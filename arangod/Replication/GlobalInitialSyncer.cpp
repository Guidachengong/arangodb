////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2017 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Simon Grätzer
////////////////////////////////////////////////////////////////////////////////

#include "GlobalInitialSyncer.h"
#include "Basics/Result.h"
#include "Basics/StringUtils.h"
#include "Basics/VelocyPackHelper.h"
#include "Replication/DatabaseInitialSyncer.h"
#include "RestServer/DatabaseFeature.h"
#include "SimpleHttpClient/SimpleHttpClient.h"
#include "SimpleHttpClient/SimpleHttpResult.h"
#include "VocBase/voc-types.h"
#include "VocBase/vocbase.h"
#include "VocBase/Methods/Databases.h"

#include <velocypack/Builder.h>
#include <velocypack/Iterator.h>
#include <velocypack/Slice.h>
#include <velocypack/velocypack-aliases.h>

using namespace arangodb;
using namespace arangodb::basics;
using namespace arangodb::httpclient;
using namespace arangodb::rest;

GlobalInitialSyncer::GlobalInitialSyncer(
  ReplicationApplierConfiguration const& configuration)
    : InitialSyncer(configuration) {
  _databaseName = TRI_VOC_SYSTEM_DATABASE;
}

GlobalInitialSyncer::~GlobalInitialSyncer() {
  try {
    sendFinishBatch();
  } catch(...) {}
}

/// @brief run method, performs a full synchronization
Result GlobalInitialSyncer::run(bool incremental) {
  if (_client == nullptr || _connection == nullptr || _endpoint == nullptr) {
    return Result(TRI_ERROR_INTERNAL, "invalid endpoint");
  }
  
  LOG_TOPIC(DEBUG, Logger::REPLICATION) << "client: getting master state";
  Result r = getMasterState();
    
  if (r.fail()) {
    return r;
  }
  
  if (_masterInfo._majorVersion > 3 ||
      (_masterInfo._majorVersion == 3 && _masterInfo._minorVersion < 3)) {
    char const* msg = "global replication is not supported with a master <  ArangoDB 3.3";
    LOG_TOPIC(WARN, Logger::REPLICATION) << msg;
    return Result(TRI_ERROR_INTERNAL, msg);
  }
  
  // create a WAL logfile barrier that prevents WAL logfile collection
  r = sendCreateBarrier(_masterInfo._lastLogTick);

  if (r.fail()) {
    return r;
  }
    
  LOG_TOPIC(DEBUG, Logger::REPLICATION) << "created logfile barrier";
  TRI_DEFER(sendRemoveBarrier());

  // start batch is required for the inventory request
  LOG_TOPIC(DEBUG, Logger::REPLICATION) << "sending start batch";
  r = sendStartBatch();
  if (r.fail()) {
    return r;
  }
  TRI_DEFER(sendFinishBatch());
  LOG_TOPIC(DEBUG, Logger::REPLICATION) << "sending start batch done";
  
  VPackBuilder builder;
  LOG_TOPIC(DEBUG, Logger::REPLICATION) << "fetching inventory";
  r  = fetchInventory(builder);
  LOG_TOPIC(DEBUG, Logger::REPLICATION) << "inventory done: " << r.errorNumber();
  if (r.fail()) {
    return r;
  }
  
  LOG_TOPIC(DEBUG, Logger::REPLICATION) << "inventory: " << builder.slice().toJson();
  VPackSlice const databases = builder.slice().get("databases");
  VPackSlice const state = builder.slice().get("state");
  if (!databases.isObject() || !state.isObject()) {
    return Result(TRI_ERROR_REPLICATION_INVALID_RESPONSE,
                  "database section or state section is missing from response or is invalid");
  }
 
  if (!_configuration._skipCreateDrop) {
    LOG_TOPIC(DEBUG, Logger::REPLICATION) << "updating server inventory"; 
    Result r = updateServerInventory(databases);
    if (r.fail()) {
      LOG_TOPIC(DEBUG, Logger::REPLICATION) << "updating server inventory failed"; 
      return r;
    }
  }
      
  LOG_TOPIC(DEBUG, Logger::REPLICATION) << "databases: " << databases.toJson();
  
  try {
    // actually sync the database
    for (auto const& database : VPackObjectIterator(databases)) {
      VPackSlice it = database.value;
      if (!it.isObject()) {
        return Result(TRI_ERROR_REPLICATION_INVALID_RESPONSE,
                      "database declaration is invalid in response");
      }
      
      VPackSlice const nameSlice = it.get("name");
      VPackSlice const idSlice = it.get("id");
      VPackSlice const collections = it.get("collections");
      if (!nameSlice.isString() ||
          !idSlice.isString() ||
          !collections.isArray()) {
        return Result(TRI_ERROR_REPLICATION_INVALID_RESPONSE,
                      "database declaration is invalid in response");
      }
      
      TRI_vocbase_t* vocbase = resolveVocbase(nameSlice);
      TRI_ASSERT(vocbase != nullptr);
      if (vocbase == nullptr) {
        return Result(TRI_ERROR_INTERNAL, "vocbase not found");
      }

      // change database name in place
      std::string const oldName = _configuration._database;
      _configuration._database = nameSlice.copyString();
      TRI_DEFER(_configuration._database = oldName);
      DatabaseInitialSyncer syncer(vocbase, _configuration);
      
      syncer.useAsChildSyncer(_masterInfo, _barrierId, _barrierUpdateTime,
                              _batchId, _batchUpdateTime);
      // run the syncer with the supplied inventory collections
      Result r = syncer.runWithInventory(false, collections);
      if (r.fail()) {
        return r;
      }
    
      // we need to pass on the update times to the next syncer
      _barrierUpdateTime = syncer.barrierUpdateTime();
      _batchUpdateTime = syncer.batchUpdateTime();
    
      sendExtendBatch();
      sendExtendBarrier();
    }
  
  } catch (...) {
    return Result(TRI_ERROR_INTERNAL, "caught an unexpected exception");
  }
  
  return TRI_ERROR_NO_ERROR;
}

/// @brief add or remove databases such that the local inventory mirrors the masters
Result GlobalInitialSyncer::updateServerInventory(VPackSlice const& masterDatabases) {
  std::set<std::string> existingDBs;
  DatabaseFeature::DATABASE->enumerateDatabases([&](TRI_vocbase_t* vocbase) {
    existingDBs.insert(vocbase->name());
  });
  
  for (auto const& database : VPackObjectIterator(masterDatabases)) {
    VPackSlice it = database.value;

    if (!it.isObject()) {
      return Result(TRI_ERROR_REPLICATION_INVALID_RESPONSE,
                    "database declaration is invalid in response");
    }
    
    VPackSlice const nameSlice = it.get("name");
    VPackSlice const idSlice = it.get("id");
    VPackSlice const collections = it.get("collections");
    if (!nameSlice.isString() ||
        !idSlice.isString() ||
        !collections.isArray()) {
      return Result(TRI_ERROR_REPLICATION_INVALID_RESPONSE,
                    "database declaration is invalid in response");
    }
    std::string const dbName = nameSlice.copyString();
    TRI_vocbase_t* vocbase = resolveVocbase(nameSlice);
    if (vocbase == nullptr) {
      // database is missing we need to create it now
      
      Result r = methods::Databases::create(dbName, VPackSlice::emptyArraySlice(),
                                            VPackSlice::emptyObjectSlice());
      if (r.fail()) {
        LOG_TOPIC(WARN, Logger::REPLICATION) << "Creating the db failed on replicant";
        return r;
      }
      vocbase = resolveVocbase(nameSlice);
      TRI_ASSERT(vocbase != nullptr); // must be loaded now
      if (vocbase == nullptr) {
        char const* msg = "DB was created with wrong id on replicant";
        LOG_TOPIC(WARN, Logger::REPLICATION) << msg;
        return Result(TRI_ERROR_INTERNAL, msg);
      }
    }
    existingDBs.erase(dbName); // remove dbs that exists on the master
    
    sendExtendBatch();
    sendExtendBarrier();
  }
  
  // all dbs left in this list no longer exist on the master
  for (std::string const& dbname : existingDBs) {
    _vocbases.erase(dbname); // make sure to release the db first
    
    TRI_vocbase_t* system = DatabaseFeature::DATABASE->systemDatabase();
    Result r = methods::Databases::drop(system, dbname);
    if (r.fail()) {
      LOG_TOPIC(WARN, Logger::REPLICATION) << "Dropping db failed on replicant";
      return r;
    }
    
    sendExtendBatch();
    sendExtendBarrier();
  }
  
  return TRI_ERROR_NO_ERROR;
}

Result GlobalInitialSyncer::fetchInventory(VPackBuilder& builder) {
  std::string url = ReplicationUrl + "/inventory?serverId=" + _localServerIdString +
  "&batchId=" + std::to_string(_batchId) + "&global=true";
  if (_configuration._includeSystem) {
    url += "&includeSystem=true";
  }
  
  // send request
  std::unique_ptr<SimpleHttpResult> response(_client->retryRequest(rest::RequestType::GET, url, nullptr, 0));
  
  if (response == nullptr || !response->isComplete()) {
    sendFinishBatch();
    return Result(TRI_ERROR_REPLICATION_NO_RESPONSE, std::string("could not connect to master at ") +
                  _masterInfo._endpoint + ": " + _client->getErrorMessage());
  }
  
  TRI_ASSERT(response != nullptr);
  
  if (response->wasHttpError()) {
    return Result(TRI_ERROR_REPLICATION_MASTER_ERROR, std::string("got invalid response from master at ") +
                  _masterInfo._endpoint + ": HTTP " + StringUtils::itoa(response->getHttpReturnCode()) + ": " +
                  response->getHttpReturnMessage());
  }

  Result r = parseResponse(builder, response.get());

  if (r.fail()) {
    return Result(r.errorNumber(), std::string("got invalid response from master at ") + _masterInfo._endpoint +
                  ": invalid response type for initial data. expecting array");
  }
  
  VPackSlice const slice = builder.slice();
  if (!slice.isObject()) {
    LOG_TOPIC(DEBUG, Logger::REPLICATION) << "client: InitialSyncer::run - inventoryResponse is not an object";
    return Result(TRI_ERROR_REPLICATION_INVALID_RESPONSE, std::string("got invalid response from master at ") +
                  _masterInfo._endpoint + ": invalid JSON");
  }

  return Result();
}

