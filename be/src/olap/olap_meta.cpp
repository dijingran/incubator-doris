// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "olap/olap_meta.h"

#include <vector>
#include <sstream>

#include "olap/olap_define.h"
#include "rocksdb/db.h"
#include "rocksdb/slice.h"
#include "rocksdb/options.h"
#include "rocksdb/slice_transform.h"
#include "common/logging.h"

using rocksdb::DB;
using rocksdb::DBOptions;
using rocksdb::ColumnFamilyDescriptor;
using rocksdb::ColumnFamilyHandle;
using rocksdb::ColumnFamilyOptions;
using rocksdb::ReadOptions;
using rocksdb::WriteOptions;
using rocksdb::Slice;
using rocksdb::Iterator;
using rocksdb::Status;
using rocksdb::kDefaultColumnFamilyName;
using rocksdb::NewFixedPrefixTransform;

namespace doris {
const std::string META_POSTFIX = "/meta";
const size_t PREFIX_LENGTH = 4;

OlapMeta::OlapMeta(const std::string& root_path)
        : _root_path(root_path),
        _db(nullptr) {
}

OlapMeta::~OlapMeta() {
    for (auto handle : _handles) {
        delete handle;
    }
    if (_db != NULL) {
        _db->Close();
        delete _db;
        _db= NULL;
    }
}

OLAPStatus OlapMeta::init() {
    // init db
    DBOptions options;
    options.IncreaseParallelism();
    options.create_if_missing = true;
    options.create_missing_column_families = true;
    std::string db_path = _root_path + META_POSTFIX;
    std::vector<ColumnFamilyDescriptor> column_families;
    // default column family is required
    column_families.emplace_back(DEFAULT_COLUMN_FAMILY, ColumnFamilyOptions());
    column_families.emplace_back(DORIS_COLUMN_FAMILY, ColumnFamilyOptions());

    // meta column family add prefix extrator to improve performance and ensure correctness
    ColumnFamilyOptions meta_column_family;
    meta_column_family.prefix_extractor.reset(NewFixedPrefixTransform(PREFIX_LENGTH));
    column_families.emplace_back(META_COLUMN_FAMILY, meta_column_family);
    Status s = DB::Open(options, db_path, column_families, &_handles, &_db);
    if (!s.ok() || _db == NULL) {
        LOG(WARNING) << "rocks db open failed, reason:" << s.ToString();
        return OLAP_ERR_META_OPEN_DB;
    }
    return OLAP_SUCCESS;
}

OLAPStatus OlapMeta::get(const int column_family_index, const std::string& key, std::string& value) {
    rocksdb::ColumnFamilyHandle* handle = _handles[column_family_index];
    Status s = _db->Get(ReadOptions(), handle, Slice(key), &value);
    if (s.IsNotFound()) {
        LOG(WARNING) << "rocks db key not found:" << key;
        return OLAP_ERR_META_KEY_NOT_FOUND;
    } else if (!s.ok()) {
        LOG(WARNING) << "rocks db get key:" << key << " failed, reason:" << s.ToString();
        return OLAP_ERR_META_GET;
    }
    return OLAP_SUCCESS;
}

OLAPStatus OlapMeta::put(const int column_family_index, const std::string& key, const std::string& value) {
    rocksdb::ColumnFamilyHandle* handle = _handles[column_family_index];
    Status s = _db->Put(WriteOptions(), handle, Slice(key), Slice(value));
    if (!s.ok()) {
        LOG(WARNING) << "rocks db put key:" << key << " failed, reason:" << s.ToString();
        return OLAP_ERR_META_PUT;
    }
    return OLAP_SUCCESS;
}

OLAPStatus OlapMeta::remove(const int column_family_index, const std::string& key) {
    rocksdb::ColumnFamilyHandle* handle = _handles[column_family_index];
    Status s = _db->Delete(WriteOptions(), handle, Slice(key));
    if (!s.ok()) {
        LOG(WARNING) << "rocks db delete key:" << key << " failed, reason:" << s.ToString();
        return OLAP_ERR_META_DELETE;
    }
    return OLAP_SUCCESS;
}

OLAPStatus OlapMeta::iterate(const int column_family_index, const std::string& prefix,
        std::function<bool(const std::string&, const std::string&)> const& func) {
    rocksdb::ColumnFamilyHandle* handle = _handles[column_family_index];
    std::unique_ptr<Iterator> it(_db->NewIterator(ReadOptions(), handle));
    if (prefix == "") {
        it->SeekToFirst();
    } else {
        it->Seek(prefix);
    }
    Status status = it->status();
    if (!status.ok()) {
        LOG(WARNING) << "rocksdb seek failed. reason:" << status.ToString();
        return OLAP_ERR_META_ITERATOR;
    }
    for (; it->Valid(); it->Next()) {
        if (prefix != "") {
            if (!it->key().starts_with(prefix)) {
                return OLAP_SUCCESS;
            }
        }
        std::string key = it->key().ToString();
        std::string value = it->value().ToString();
        bool ret = func(key, value);
        if (!ret) {
            break;
        }
    }
    return OLAP_SUCCESS;
}

std::string OlapMeta::get_root_path() {
    return _root_path;
}

}