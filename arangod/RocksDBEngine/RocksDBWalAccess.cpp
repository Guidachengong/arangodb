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

#include "RocksDBEngine/RocksDBWalAccess.h"
#include "Basics/StaticStrings.h"
#include "RestServer/DatabaseFeature.h"
#include "RocksDBEngine/RocksDBColumnFamily.h"
#include "RocksDBEngine/RocksDBCommon.h"
#include "RocksDBEngine/RocksDBLogValue.h"
#include "RocksDBEngine/RocksDBReplicationTailing.h"
#include "RocksDBEngine/RocksDBTypes.h"
#include "VocBase/LogicalCollection.h"

#include "Logger/Logger.h"

#include <rocksdb/utilities/transaction_db.h>
#include <velocypack/Builder.h>

using namespace arangodb;

/// {"tickMin":"123", "tickMax":"456", "version":"3.2", "serverId":"abc"}
Result RocksDBWalAccess::tickRange(
    std::pair<TRI_voc_tick_t, TRI_voc_tick_t>& minMax) const {
  rocksdb::TransactionDB* tdb = rocksutils::globalRocksDB();
  rocksdb::VectorLogPtr walFiles;
  rocksdb::Status s = tdb->GetSortedWalFiles(walFiles);
  if (!s.ok()) {
    return rocksutils::convertStatus(s);
  }

  if (walFiles.size() > 0) {
    minMax.first = walFiles.front()->StartSequence();
  }
  minMax.second = tdb->GetLatestSequenceNumber();
  return TRI_ERROR_NO_ERROR;
}

/// {"lastTick":"123",
///  "version":"3.2",
///  "serverId":"abc",
///  "clients": {
///    "serverId": "ass", "lastTick":"123", ...
///  }}
///
TRI_voc_tick_t RocksDBWalAccess::lastTick() const {
  rocksutils::globalRocksEngine()->syncWal();
  return rocksutils::globalRocksDB()->GetLatestSequenceNumber();
}

/// WAL parser
class MyWALParser : public rocksdb::WriteBatch::Handler {
 public:
  MyWALParser(bool includeSystem, WalAccess::WalFilter const& filter,
              WalAccess::MarkerCallback const& f)
      : _documentsCF(RocksDBColumnFamily::documents()->GetID()),
        _definitionsCF(RocksDBColumnFamily::definitions()->GetID()),
        _include(filter),
        _includeSystem(includeSystem),
        _callback(f),
        _startSequence(0),
        _currentSequence(0),
        _responseSize(0) {}

  ~MyWALParser() {
    for (auto const& pair : _vocbaseCache) {
      pair.second->release();
    }
  }

  void LogData(rocksdb::Slice const& blob) override {
    RocksDBLogType type = RocksDBLogValue::type(blob);
    TRI_DEFER(_lastLogType = type);

    tick();
    switch (type) {
      case RocksDBLogType::DatabaseCreate:
      case RocksDBLogType::DatabaseDrop: {
        _currentDbId = RocksDBLogValue::databaseId(blob);
        break;
      }
      case RocksDBLogType::CollectionRename:
      case RocksDBLogType::CollectionCreate:
      case RocksDBLogType::CollectionChange:
      case RocksDBLogType::CollectionDrop: {
        if (_lastLogType == RocksDBLogType::IndexCreate) {
          TRI_ASSERT(_currentDbId == RocksDBLogValue::databaseId(blob));
          TRI_ASSERT(_currentCid == RocksDBLogValue::collectionId(blob));
        }
        _currentDbId = RocksDBLogValue::databaseId(blob);
        _currentCid = RocksDBLogValue::collectionId(blob);
        break;
      }
      case RocksDBLogType::IndexCreate: {
        _currentDbId = RocksDBLogValue::databaseId(blob);
        _currentCid = RocksDBLogValue::collectionId(blob);
        // only print markers from this collection if it is set
        if (shouldHandleCollection(_currentDbId, _currentCid)) {
          {
            VPackObjectBuilder marker(&_builder, true);
            marker->add("type", VPackValue(rocksutils::convertLogType(type)));
            marker->add("database", VPackValue(std::to_string(_currentDbId)));
            marker->add("uuid",
                        VPackValue(cidToUUID(_currentDbId, _currentCid)));
            marker->add("data", RocksDBLogValue::indexSlice(blob));
          }
          _callback(loadVocbase(_currentDbId), _builder.slice());
          _builder.clear();
        }
        break;
      }
      case RocksDBLogType::IndexDrop: {
        _currentDbId = RocksDBLogValue::databaseId(blob);
        _currentCid = RocksDBLogValue::collectionId(blob);
        TRI_idx_iid_t iid = RocksDBLogValue::indexId(blob);
        // only print markers from this collection if it is set
        if (shouldHandleCollection(_currentDbId, _currentCid)) {
          {
            VPackObjectBuilder marker(&_builder, true);
            marker->add("type", VPackValue(rocksutils::convertLogType(type)));
            marker->add("database", VPackValue(std::to_string(_currentDbId)));
            marker->add("cid", VPackValue(std::to_string(_currentCid)));
            VPackObjectBuilder data(&_builder, "data", true);
            data->add("id", VPackValue(std::to_string(iid)));
          }
          _callback(loadVocbase(_currentDbId), _builder.slice());
          _builder.clear();
        }
        break;
      }
      case RocksDBLogType::ViewCreate:
      case RocksDBLogType::ViewChange:
      case RocksDBLogType::ViewDrop: {
        // TODO
        break;
      }
      case RocksDBLogType::BeginTransaction: {
        TRI_ASSERT(!_singleOp);
        _seenBeginTransaction = true;
        _currentDbId = RocksDBLogValue::databaseId(blob);
        _currentTrxId = RocksDBLogValue::transactionId(blob);
        {
          VPackObjectBuilder marker(&_builder, true);
          marker->add("tick", VPackValue(std::to_string(_currentSequence)));
          marker->add("type", VPackValue(rocksutils::convertLogType(type)));
          marker->add("database", VPackValue(std::to_string(_currentDbId)));
          marker->add("tid", VPackValue(std::to_string(_currentTrxId)));
        }
        _callback(loadVocbase(_currentDbId), _builder.slice());
        _builder.clear();
        break;
      }
      case RocksDBLogType::DocumentOperationsPrologue: {
        _currentCid = RocksDBLogValue::collectionId(blob);
        break;
      }
      case RocksDBLogType::DocumentRemove: {
        _removeDocumentKey = RocksDBLogValue::documentKey(blob).toString();
        break;
      }
      case RocksDBLogType::SingleRemove: {
        _removeDocumentKey = RocksDBLogValue::documentKey(blob).toString();
        // intentional fall through
      }
      case RocksDBLogType::SinglePut: {
        writeCommitMarker();
        _singleOp = true;
        _currentDbId = RocksDBLogValue::databaseId(blob);
        _currentCid = RocksDBLogValue::collectionId(blob);
        _currentTrxId = 0;
        break;
      }

      default:
        LOG_TOPIC(WARN, Logger::REPLICATION) << "Unhandled wal log entry "
                                             << rocksDBLogTypeName(type);
        break;
    }
  }

  rocksdb::Status PutCF(uint32_t column_family_id, rocksdb::Slice const& key,
                        rocksdb::Slice const& value) override {
    tick();
    if (!shouldHandleMarker(column_family_id, key)) {
      return rocksdb::Status();
    }

    if (column_family_id == _definitionsCF) {
      if (RocksDBKey::type(key) == RocksDBEntryType::Database) {
        TRI_ASSERT(_lastLogType == RocksDBLogType::DatabaseCreate ||
                   _lastLogType == RocksDBLogType::DatabaseDrop);
        {
          VPackObjectBuilder marker(&_builder, true);
          marker->add("tick", VPackValue(std::to_string(_currentSequence)));
          marker->add("type",
                      VPackValue(rocksutils::convertLogType(_lastLogType)));
          marker->add("database", VPackValue(std::to_string(_currentDbId)));
          marker->add("data", RocksDBValue::data(value));
        }
        _callback(loadVocbase(_currentDbId), _builder.slice());
        _builder.clear();

      } else if (RocksDBKey::type(key) == RocksDBEntryType::Collection) {
        if (_lastLogType == RocksDBLogType::IndexCreate ||
            _lastLogType == RocksDBLogType::IndexDrop) {
          _lastLogType = RocksDBLogType::Invalid;
          return rocksdb::Status();
        }
        TRI_ASSERT(_lastLogType == RocksDBLogType::CollectionCreate ||
                   _lastLogType == RocksDBLogType::CollectionChange ||
                   _lastLogType == RocksDBLogType::CollectionRename);
        TRI_ASSERT(_currentDbId != 0 && _currentCid != 0);
        {
          VPackObjectBuilder marker(&_builder, true);
          marker->add("tick", VPackValue(std::to_string(_currentSequence)));
          marker->add("type",
                      VPackValue(rocksutils::convertLogType(_lastLogType)));
          marker->add("database", VPackValue(std::to_string(_currentDbId)));
          marker->add("uuid", VPackValue(cidToUUID(_currentDbId, _currentCid)));
          if (_lastLogType == RocksDBLogType::CollectionRename) {
            VPackObjectBuilder data(&_builder, "data", true);
            data->add("id", VPackValue(std::to_string(_currentCid)));
            data->add("name", VPackValue(cidToName(_currentDbId, _currentCid)));
          } else {  // change and create need full data
            VPackSlice data = RocksDBValue::data(value);
            marker->add("data", data);
          }
        }
        _callback(loadVocbase(_currentDbId), _builder.slice());
        _builder.clear();

        // log type is only ever relevant, immediately after it appeared
        // we want double occurences create / drop / change collection to fail
        _lastLogType = RocksDBLogType::Invalid;
        _currentDbId = 0;
        _currentCid = 0;
      }  // if (RocksDBKey::type(key) == RocksDBEntryType::Collection)

    } else if (column_family_id == _documentsCF) {
      TRI_ASSERT((_seenBeginTransaction && !_singleOp) ||
                 (!_seenBeginTransaction && _singleOp));
      // if real transaction, we need the trx id
      TRI_ASSERT(!_seenBeginTransaction || _currentTrxId != 0);
      TRI_ASSERT(_currentDbId != 0 && _currentCid != 0);
      {
        VPackObjectBuilder marker(&_builder, true);
        marker->add("tick", VPackValue(std::to_string(_currentSequence)));
        marker->add("type", VPackValue(REPLICATION_MARKER_DOCUMENT));
        // auto containers = getContainerIds(key);
        marker->add("database", VPackValue(std::to_string(_currentDbId)));
        marker->add("cid", VPackValue(std::to_string(_currentCid)));
        if (_singleOp) {  // single op is defined to have a transaction id of 0
          marker->add("tid", VPackValue("0"));
          _singleOp = false;
        } else {
          marker->add("tid", VPackValue(std::to_string(_currentTrxId)));
        }
        marker->add("data", RocksDBValue::data(value));
      }
      _callback(loadVocbase(_currentDbId), _builder.slice());
      _builder.clear();
    }

    return rocksdb::Status();
  }

  rocksdb::Status DeleteCF(uint32_t column_family_id,
                           rocksdb::Slice const& key) override {
    return handleDeletion(column_family_id, key);
  }

  rocksdb::Status SingleDeleteCF(uint32_t column_family_id,
                                 rocksdb::Slice const& key) override {
    return handleDeletion(column_family_id, key);
  }

  rocksdb::Status handleDeletion(uint32_t column_family_id,
                                 rocksdb::Slice const& key) {
    tick();
    if (!shouldHandleMarker(column_family_id, key)) {
      return rocksdb::Status();
    }

    if (column_family_id == _definitionsCF &&
        RocksDBKey::type(key) == RocksDBEntryType::Collection) {
      // a database DROP will not set this flag
      if (_lastLogType == RocksDBLogType::CollectionDrop) {
        TRI_ASSERT(_currentDbId != 0 && _currentCid != 0);
        {
          VPackObjectBuilder marker(&_builder, true);
          marker->add("tick", VPackValue(std::to_string(_currentSequence)));
          marker->add("type", VPackValue(REPLICATION_COLLECTION_DROP));
          marker->add("database", VPackValue(std::to_string(_currentDbId)));
          marker->add("uuid", VPackValue(cidToUUID(_currentDbId, _currentCid)));
          marker->add("data", VPackValue(VPackValueType::Object));
          VPackObjectBuilder data(&_builder, "data", true);
          data->add("id", VPackValue(std::to_string(_currentCid)));
          data->add("name", VPackValue(""));  // not used at all
        }
        _callback(loadVocbase(_currentDbId), _builder.slice());
        _builder.clear();
      }
    } else if (column_family_id == _documentsCF) {
      // document removes, because of a drop is not transactional and
      // should not appear in the WAL. Allso fixes
      if (!(_seenBeginTransaction || _singleOp)) {
        return rocksdb::Status();
      }
      // TODO somehow fix counters if we optimize the DELETE in
      // documentRemove on updates
      if (_lastLogType != RocksDBLogType::DocumentRemove &&
          _lastLogType != RocksDBLogType::SingleRemove) {
        return rocksdb::Status();
      }
      // TRI_ASSERT(_lastLogType == RocksDBLogType::DocumentRemove ||
      //           _lastLogType == RocksDBLogType::SingleRemove);
      TRI_ASSERT(!_seenBeginTransaction || _currentTrxId != 0);
      TRI_ASSERT(_currentDbId != 0 && _currentCid != 0);
      TRI_ASSERT(!_removeDocumentKey.empty());
      {
        uint64_t revId =
            RocksDBKey::revisionId(RocksDBEntryType::Document, key);
        VPackObjectBuilder marker(&_builder, true);
        marker->add("tick", VPackValue(std::to_string(_currentSequence)));
        marker->add("type", VPackValue(REPLICATION_MARKER_REMOVE));
        marker->add("database", VPackValue(std::to_string(_currentDbId)));
        marker->add("uuid", VPackValue(std::to_string(_currentCid)));
        if (_singleOp) {  // single op is defined to 0
          marker->add("tid", VPackValue("0"));
          _singleOp = false;
        } else {
          marker->add("tid", VPackValue(std::to_string(_currentTrxId)));
        }
        VPackObjectBuilder data(&_builder, "data", true);
        data->add(StaticStrings::KeyString, VPackValue(_removeDocumentKey));
        data->add(StaticStrings::RevString, VPackValue(std::to_string(revId)));
      }
      _callback(loadVocbase(_currentDbId), _builder.slice());
      _builder.clear();
      _removeDocumentKey.clear();
    }
    return rocksdb::Status();
  }

  void startNewBatch(rocksdb::SequenceNumber startSequence) {
    // starting new write batch
    _startSequence = startSequence;
    _currentSequence = startSequence;
  }

  void writeCommitMarker() {
    if (_seenBeginTransaction) {
      _builder.openObject();
      _builder.add("tick", VPackValue(std::to_string(_currentSequence)));
      _builder.add(
          "type",
          VPackValue(static_cast<uint64_t>(REPLICATION_TRANSACTION_COMMIT)));
      _builder.add("database", VPackValue(std::to_string(_currentDbId)));
      _builder.add("tid", VPackValue(std::to_string(_currentTrxId)));
      _builder.close();
    }
    // rest all states
    _lastLogType = RocksDBLogType::Invalid;
    _seenBeginTransaction = false;
    _singleOp = false;
    _startOfBatch = true;
    _currentDbId = 0;
    _currentTrxId = 0;
    _currentCid = 0;
    _dropCollectionUUID.clear();
    _removeDocumentKey.clear();
    _indexSlice = VPackSlice::illegalSlice();
  }

  uint64_t endBatch() {
    writeCommitMarker();
    _removeDocumentKey.clear();
    return _currentSequence;
  }

  size_t responseSize() const { return _responseSize; }

 private:
  // tick function that is called before each new WAL entry
  void tick() {
    if (_startOfBatch) {
      // we are at the start of a batch. do NOT increase sequence number
      _startOfBatch = false;
    } else {
      // we are inside a batch already. now increase sequence number
      ++_currentSequence;
    }
  }

  bool shouldHandleMarker(uint32_t column_family_id,
                          rocksdb::Slice const& key) {
    TRI_voc_cid_t cid;
    if (column_family_id == _definitionsCF) {
      if (RocksDBKey::type(key) == RocksDBEntryType::Database) {
        return true;
      } else if (RocksDBKey::type(key) == RocksDBEntryType::Collection ||
                 RocksDBKey::type(key) == RocksDBEntryType::View) {
        cid = RocksDBKey::collectionId(key);
      } else {
        return false;
      }
    } else if (column_family_id == _documentsCF) {
      cid = _currentCid;
    } else {
      return false;
    }

    if (!_include.empty() && !shouldHandleCollection(_currentDbId, cid)) {
      return false;
    }

    if (_lastLogType != RocksDBLogType::CollectionDrop) {
      // no document removes of dropped collections
      TRI_vocbase_t* vocbase = loadVocbase(_currentDbId);
      std::string const collectionName = vocbase->collectionName(cid);
      if (collectionName.empty()) {
        return false;
      }
      if (!_includeSystem && collectionName[0] == '_') {
        return false;
      }
      if (TRI_ExcludeCollectionReplication(collectionName.c_str(),
                                           _includeSystem)) {
        return false;
      }
    }
    return true;
  }

  bool shouldHandleCollection(TRI_voc_tick_t dbid, TRI_voc_cid_t cid) {
    auto const& it = _include.find(dbid);
    if (it != _include.end()) {
      auto const& it2 = it->second.find(cid);
      return it2 != it->second.end();
    }
    return false;
  }

  TRI_vocbase_t* loadVocbase(TRI_voc_tick_t dbid) {
    auto const& it = _vocbaseCache.find(dbid);
    if (it == _vocbaseCache.end()) {
      TRI_vocbase_t* vocbase = DatabaseFeature::DATABASE->useDatabase(dbid);
      _vocbaseCache.emplace(dbid, vocbase);
      return vocbase;
    } else {
      return it->second;
    }
  }

  std::string const& cidToUUID(TRI_voc_tick_t dbid, TRI_voc_cid_t cid) {
    auto const& uuid = _uuidCache.find(cid);
    if (uuid != _uuidCache.end()) {
      return uuid->second;
    }

    TRI_vocbase_t* vocbase = loadVocbase(dbid);
    LogicalCollection* collection = vocbase->lookupCollection(cid);
    /*if (collection->deleted()) {}*/
    _uuidCache.emplace(cid, collection->globallyUniqueId());
    return _uuidCache[cid];
  }

  std::string cidToName(TRI_voc_tick_t dbid, TRI_voc_cid_t cid) {
    TRI_vocbase_t* vocbase = loadVocbase(dbid);
    return vocbase->collectionName(cid);
  }

 private:
  uint32_t const _documentsCF;
  uint32_t const _definitionsCF;

  /// arbitrary collection filter (inclusive)
  WalAccess::WalFilter _include;
  /// ignore system collections
  bool const _includeSystem;

  /// result builder
  VPackBuilder _builder;
  WalAccess::MarkerCallback _callback;

  /// cache the vocbases
  std::map<TRI_voc_tick_t, TRI_vocbase_t*> _vocbaseCache;
  // collection replication UUID cache
  std::map<TRI_voc_cid_t, std::string> _uuidCache;

  rocksdb::SequenceNumber _startSequence;
  rocksdb::SequenceNumber _currentSequence;
  size_t _responseSize;

  // Various state machine flags
  RocksDBLogType _lastLogType = RocksDBLogType::Invalid;
  bool _seenBeginTransaction = false;
  bool _singleOp = false;
  bool _startOfBatch = false;
  TRI_voc_tick_t _currentDbId = 0;
  TRI_voc_tick_t _currentTrxId = 0;
  TRI_voc_cid_t _currentCid = 0;
  std::string _dropCollectionUUID;
  std::string _removeDocumentKey;
  VPackSlice _indexSlice;
};

// iterates over WAL starting at 'from' and returns up to 'chunkSize' documents
// from the corresponding database
WalTailingResult RocksDBWalAccess::tail(uint64_t tickStart, uint64_t tickEnd,
                                        size_t chunkSize, bool includeSystem,
                                        WalFilter const& filter,
                                        MarkerCallback const& func) const {
  uint64_t firstTick = tickStart;
  uint64_t lastTick = tickStart;
  uint64_t lastWrittenTick = tickStart;

  auto handler = std::make_unique<MyWALParser>(includeSystem, filter, func);
  std::unique_ptr<rocksdb::TransactionLogIterator> iterator;  // reader();

  rocksdb::TransactionLogIterator::ReadOptions ro(false);
  rocksdb::Status s =
      rocksutils::globalRocksDB()->GetUpdatesSince(tickStart, &iterator, ro);
  if (!s.ok()) {
    Result r = convertStatus(s, rocksutils::StatusHint::wal);
    return WalTailingResult{r.errorNumber(), 0, 0};
  }

  // we need to check if the builder is bigger than the chunksize,
  // only after we printed a full WriteBatch. Otherwise a client might
  // never read the full writebatch
  while (iterator->Valid() && lastTick <= tickEnd &&
         handler->responseSize() < chunkSize) {
    s = iterator->status();
    if (!s.ok()) {
      LOG_TOPIC(ERR, Logger::ENGINES) << "error during WAL scan";
      auto converted = rocksutils::convertStatus(s);
      return WalTailingResult(converted.errorNumber(), tickStart,
                              lastWrittenTick);
    }

    rocksdb::BatchResult batch = iterator->GetBatch();
    TRI_ASSERT(lastTick == tickStart || batch.sequence >= lastTick);
    if (batch.sequence <= tickStart) {
      iterator->Next();  // skip
      continue;
    } else if (batch.sequence > tickEnd) {
      break;  // cancel out
    }

    // record the first tick we are reading
    if (firstTick == tickStart) {
      firstTick = batch.sequence;
    }
    lastTick = batch.sequence;
    handler->startNewBatch(batch.sequence);
    s = batch.writeBatchPtr->Iterate(handler.get());
    if (!s.ok()) {
      LOG_TOPIC(ERR, Logger::ROCKSDB) << s.ToString();
      break;
    }

    lastWrittenTick = handler->endBatch();
    handler->endBatch();
    iterator->Next();
  }

  if (!s.ok()) {  // TODO do something?
    Result r = convertStatus(s, rocksutils::StatusHint::wal);
    return WalTailingResult(r.errorNumber(), firstTick, lastWrittenTick);
  }
  return WalTailingResult(TRI_ERROR_NO_ERROR, firstTick, lastWrittenTick);
}
