// Copyright (C) 2019-2020 Zilliz. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except in compliance
// with the License. You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied. See the License for the specific language governing permissions and limitations under the License

#include "tantivy-binding.h"
#include "common/Slice.h"
#include "common/RegexQuery.h"
#include "storage/LocalChunkManagerSingleton.h"
#include "index/InvertedIndexTantivy.h"
#include "index/InvertedIndexUtil.h"
#include "log/Log.h"
#include "index/Utils.h"
#include "storage/Util.h"

#include <boost/filesystem.hpp>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <cstddef>
#include <vector>
#include "InvertedIndexTantivy.h"

namespace milvus::index {
constexpr const char* TMP_INVERTED_INDEX_PREFIX = "/tmp/milvus/inverted-index/";

inline TantivyDataType
get_tantivy_data_type(const proto::schema::FieldSchema& schema) {
    switch (schema.data_type()) {
        case proto::schema::Array:
            return get_tantivy_data_type(schema.element_type());
        default:
            return get_tantivy_data_type(schema.data_type());
    }
}

template <typename T>
void
InvertedIndexTantivy<T>::InitForBuildIndex() {
    auto field =
        std::to_string(disk_file_manager_->GetFieldDataMeta().field_id);
    auto prefix = disk_file_manager_->GetIndexIdentifier();
    path_ = std::string(TMP_INVERTED_INDEX_PREFIX) + prefix;
    boost::filesystem::create_directories(path_);
    d_type_ = get_tantivy_data_type(schema_);
    if (tantivy_index_exist(path_.c_str())) {
        PanicInfo(IndexBuildError,
                  "build inverted index temp dir:{} not empty",
                  path_);
    }
    wrapper_ = std::make_shared<TantivyIndexWrapper>(
        field.c_str(), d_type_, path_.c_str(), inverted_index_single_segment_);
}

template <typename T>
InvertedIndexTantivy<T>::InvertedIndexTantivy(
    const storage::FileManagerContext& ctx, bool inverted_index_single_segment)
    : ScalarIndex<T>(INVERTED_INDEX_TYPE),
      schema_(ctx.fieldDataMeta.field_schema),
      inverted_index_single_segment_(inverted_index_single_segment) {
    mem_file_manager_ = std::make_shared<MemFileManager>(ctx);
    disk_file_manager_ = std::make_shared<DiskFileManager>(ctx);
    // push init wrapper to load process
    if (ctx.for_loading_index) {
        return;
    }
    InitForBuildIndex();
}

template <typename T>
InvertedIndexTantivy<T>::~InvertedIndexTantivy() {
    if (wrapper_) {
        wrapper_->free();
    }
    auto local_chunk_manager =
        storage::LocalChunkManagerSingleton::GetInstance().GetChunkManager();
    auto prefix = path_;
    LOG_INFO("inverted index remove path:{}", path_);
    local_chunk_manager->RemoveDir(prefix);
}

template <typename T>
void
InvertedIndexTantivy<T>::finish() {
    wrapper_->finish();
}

template <typename T>
BinarySet
InvertedIndexTantivy<T>::Serialize(const Config& config) {
    folly::SharedMutex::ReadHolder lock(mutex_);
    auto index_valid_data_length = null_offset_.size() * sizeof(size_t);
    std::shared_ptr<uint8_t[]> index_valid_data(
        new uint8_t[index_valid_data_length]);
    memcpy(
        index_valid_data.get(), null_offset_.data(), index_valid_data_length);
    lock.unlock();
    BinarySet res_set;
    if (index_valid_data_length > 0) {
        res_set.Append(
            "index_null_offset", index_valid_data, index_valid_data_length);
    }
    milvus::Disassemble(res_set);
    return res_set;
}

template <typename T>
IndexStatsPtr
InvertedIndexTantivy<T>::Upload(const Config& config) {
    finish();

    boost::filesystem::path p(path_);
    boost::filesystem::directory_iterator end_iter;

    for (boost::filesystem::directory_iterator iter(p); iter != end_iter;
         iter++) {
        if (boost::filesystem::is_directory(*iter)) {
            LOG_WARN("{} is a directory", iter->path().string());
        } else {
            LOG_INFO("trying to add index file: {}", iter->path().string());
            AssertInfo(disk_file_manager_->AddFile(iter->path().string()),
                       "failed to add index file: {}",
                       iter->path().string());
            LOG_INFO("index file: {} added", iter->path().string());
        }
    }

    auto remote_paths_to_size = disk_file_manager_->GetRemotePathsToFileSize();

    auto binary_set = Serialize(config);
    mem_file_manager_->AddFile(binary_set);
    auto remote_mem_path_to_size =
        mem_file_manager_->GetRemotePathsToFileSize();

    std::vector<SerializedIndexFileInfo> index_files;
    index_files.reserve(remote_paths_to_size.size() +
                        remote_mem_path_to_size.size());
    for (auto& file : remote_paths_to_size) {
        index_files.emplace_back(file.first, file.second);
    }
    for (auto& file : remote_mem_path_to_size) {
        index_files.emplace_back(file.first, file.second);
    }
    return IndexStats::New(mem_file_manager_->GetAddedTotalMemSize() +
                               disk_file_manager_->GetAddedTotalFileSize(),
                           std::move(index_files));
}

template <typename T>
void
InvertedIndexTantivy<T>::Build(const Config& config) {
    auto insert_files =
        GetValueFromConfig<std::vector<std::string>>(config, "insert_files");
    AssertInfo(insert_files.has_value(), "insert_files were empty");
    auto field_datas =
        mem_file_manager_->CacheRawDataToMemory(insert_files.value());
    BuildWithFieldData(field_datas);
}

template <typename T>
void
InvertedIndexTantivy<T>::Load(milvus::tracer::TraceContext ctx,
                              const Config& config) {
    auto index_files =
        GetValueFromConfig<std::vector<std::string>>(config, "index_files");
    AssertInfo(index_files.has_value(),
               "index file paths is empty when load disk ann index data");
    auto prefix = disk_file_manager_->GetLocalIndexObjectPrefix();
    auto files_value = index_files.value();
    // need erase the index type file that has been readed
    auto index_type_file =
        disk_file_manager_->GetRemoteIndexPrefix() + std::string("/index_type");
    files_value.erase(std::remove_if(files_value.begin(),
                                     files_value.end(),
                                     [&](const std::string& file) {
                                         return file == index_type_file;
                                     }),
                      files_value.end());

    auto it = std::find_if(
        files_value.begin(), files_value.end(), [](const std::string& file) {
            return file.substr(file.find_last_of('/') + 1) ==
                   "index_null_offset";
        });
    if (it != files_value.end()) {
        std::vector<std::string> file;
        file.push_back(*it);
        files_value.erase(it);
        auto index_datas = mem_file_manager_->LoadIndexToMemory(file);
        AssembleIndexDatas(index_datas);
        BinarySet binary_set;
        for (auto& [key, data] : index_datas) {
            auto size = data->DataSize();
            auto deleter = [&](uint8_t*) {};  // avoid repeated deconstruction
            auto buf = std::shared_ptr<uint8_t[]>(
                (uint8_t*)const_cast<void*>(data->Data()), deleter);
            binary_set.Append(key, buf, size);
        }
        auto index_valid_data = binary_set.GetByName("index_null_offset");
        folly::SharedMutex::WriteHolder lock(mutex_);
        null_offset_.resize((size_t)index_valid_data->size / sizeof(size_t));
        memcpy(null_offset_.data(),
               index_valid_data->data.get(),
               (size_t)index_valid_data->size);
    }
    disk_file_manager_->CacheIndexToDisk(files_value);
    path_ = prefix;
    wrapper_ = std::make_shared<TantivyIndexWrapper>(prefix.c_str());
}

template <typename T>
const TargetBitmap
InvertedIndexTantivy<T>::In(size_t n, const T* values) {
    TargetBitmap bitset(Count());
    for (size_t i = 0; i < n; ++i) {
        auto array = wrapper_->term_query(values[i]);
        apply_hits(bitset, array, true);
    }
    return bitset;
}

template <typename T>
const TargetBitmap
InvertedIndexTantivy<T>::IsNull() {
    int64_t count = Count();
    TargetBitmap bitset(count);
    folly::SharedMutex::ReadHolder lock(mutex_);
    auto end =
        std::lower_bound(null_offset_.begin(), null_offset_.end(), count);
    for (auto iter = null_offset_.begin(); iter != end; ++iter) {
        bitset.set(*iter);
    }
    return bitset;
}

template <typename T>
const TargetBitmap
InvertedIndexTantivy<T>::IsNotNull() {
    int64_t count = Count();
    TargetBitmap bitset(count, true);
    folly::SharedMutex::ReadHolder lock(mutex_);
    auto end =
        std::lower_bound(null_offset_.begin(), null_offset_.end(), count);
    for (auto iter = null_offset_.begin(); iter != end; ++iter) {
        bitset.reset(*iter);
    }
    return bitset;
}

template <typename T>
const TargetBitmap
InvertedIndexTantivy<T>::InApplyFilter(
    size_t n, const T* values, const std::function<bool(size_t)>& filter) {
    TargetBitmap bitset(Count());
    for (size_t i = 0; i < n; ++i) {
        auto array = wrapper_->term_query(values[i]);
        apply_hits_with_filter(bitset, array, filter);
    }
    return bitset;
}

template <typename T>
void
InvertedIndexTantivy<T>::InApplyCallback(
    size_t n, const T* values, const std::function<void(size_t)>& callback) {
    for (size_t i = 0; i < n; ++i) {
        auto array = wrapper_->term_query(values[i]);
        apply_hits_with_callback(array, callback);
    }
}

template <typename T>
const TargetBitmap
InvertedIndexTantivy<T>::NotIn(size_t n, const T* values) {
    int64_t count = Count();
    TargetBitmap bitset(count, true);
    for (size_t i = 0; i < n; ++i) {
        auto array = wrapper_->term_query(values[i]);
        apply_hits(bitset, array, false);
    }

    folly::SharedMutex::ReadHolder lock(mutex_);
    auto end =
        std::lower_bound(null_offset_.begin(), null_offset_.end(), count);
    for (auto iter = null_offset_.begin(); iter != end; ++iter) {
        bitset.reset(*iter);
    }
    return bitset;
}

template <typename T>
const TargetBitmap
InvertedIndexTantivy<T>::Range(T value, OpType op) {
    TargetBitmap bitset(Count());
    switch (op) {
        case OpType::LessThan: {
            auto array = wrapper_->upper_bound_range_query(value, false);
            apply_hits(bitset, array, true);
        } break;
        case OpType::LessEqual: {
            auto array = wrapper_->upper_bound_range_query(value, true);
            apply_hits(bitset, array, true);
        } break;
        case OpType::GreaterThan: {
            auto array = wrapper_->lower_bound_range_query(value, false);
            apply_hits(bitset, array, true);
        } break;
        case OpType::GreaterEqual: {
            auto array = wrapper_->lower_bound_range_query(value, true);
            apply_hits(bitset, array, true);
        } break;
        default:
            PanicInfo(OpTypeInvalid,
                      fmt::format("Invalid OperatorType: {}", op));
    }

    return bitset;
}

template <typename T>
const TargetBitmap
InvertedIndexTantivy<T>::Range(T lower_bound_value,
                               bool lb_inclusive,
                               T upper_bound_value,
                               bool ub_inclusive) {
    TargetBitmap bitset(Count());
    auto array = wrapper_->range_query(
        lower_bound_value, upper_bound_value, lb_inclusive, ub_inclusive);
    apply_hits(bitset, array, true);
    return bitset;
}

template <typename T>
const TargetBitmap
InvertedIndexTantivy<T>::PrefixMatch(const std::string_view prefix) {
    TargetBitmap bitset(Count());
    std::string s(prefix);
    auto array = wrapper_->prefix_query(s);
    apply_hits(bitset, array, true);
    return bitset;
}

template <typename T>
const TargetBitmap
InvertedIndexTantivy<T>::Query(const DatasetPtr& dataset) {
    return ScalarIndex<T>::Query(dataset);
}

template <>
const TargetBitmap
InvertedIndexTantivy<std::string>::Query(const DatasetPtr& dataset) {
    auto op = dataset->Get<OpType>(OPERATOR_TYPE);
    if (op == OpType::PrefixMatch) {
        auto prefix = dataset->Get<std::string>(PREFIX_VALUE);
        return PrefixMatch(prefix);
    }
    return ScalarIndex<std::string>::Query(dataset);
}

template <typename T>
const TargetBitmap
InvertedIndexTantivy<T>::RegexQuery(const std::string& regex_pattern) {
    TargetBitmap bitset(Count());
    auto array = wrapper_->regex_query(regex_pattern);
    apply_hits(bitset, array, true);
    return bitset;
}

template <typename T>
void
InvertedIndexTantivy<T>::BuildWithRawDataForUT(size_t n,
                                               const void* values,
                                               const Config& config) {
    if constexpr (std::is_same_v<bool, T>) {
        schema_.set_data_type(proto::schema::DataType::Bool);
    }
    if constexpr (std::is_same_v<int8_t, T>) {
        schema_.set_data_type(proto::schema::DataType::Int8);
    }
    if constexpr (std::is_same_v<int16_t, T>) {
        schema_.set_data_type(proto::schema::DataType::Int16);
    }
    if constexpr (std::is_same_v<int32_t, T>) {
        schema_.set_data_type(proto::schema::DataType::Int32);
    }
    if constexpr (std::is_same_v<int64_t, T>) {
        schema_.set_data_type(proto::schema::DataType::Int64);
    }
    if constexpr (std::is_same_v<float, T>) {
        schema_.set_data_type(proto::schema::DataType::Float);
    }
    if constexpr (std::is_same_v<double, T>) {
        schema_.set_data_type(proto::schema::DataType::Double);
    }
    if constexpr (std::is_same_v<std::string, T>) {
        schema_.set_data_type(proto::schema::DataType::VarChar);
    }
    boost::uuids::random_generator generator;
    auto uuid = generator();
    auto prefix = boost::uuids::to_string(uuid);
    path_ = fmt::format("/tmp/{}", prefix);
    boost::filesystem::create_directories(path_);
    d_type_ = get_tantivy_data_type(schema_);
    std::string field = "test_inverted_index";
    inverted_index_single_segment_ =
        GetValueFromConfig<int32_t>(config,
                                    milvus::index::SCALAR_INDEX_ENGINE_VERSION)
            .value_or(1) == 0;
    wrapper_ = std::make_shared<TantivyIndexWrapper>(
        field.c_str(), d_type_, path_.c_str(), inverted_index_single_segment_);
    if (!inverted_index_single_segment_) {
        if (config.find("is_array") != config.end()) {
            // only used in ut.
            auto arr = static_cast<const boost::container::vector<T>*>(values);
            for (size_t i = 0; i < n; i++) {
                wrapper_->template add_array_data(
                    arr[i].data(), arr[i].size(), i);
            }
        } else {
            wrapper_->add_data<T>(static_cast<const T*>(values), n, 0);
        }
    } else {
        if (config.find("is_array") != config.end()) {
            // only used in ut.
            auto arr = static_cast<const boost::container::vector<T>*>(values);
            for (size_t i = 0; i < n; i++) {
                wrapper_->template add_array_data_by_single_segment_writer(
                    arr[i].data(), arr[i].size());
            }
        } else {
            wrapper_->add_data_by_single_segment_writer<T>(
                static_cast<const T*>(values), n);
        }
    }
    wrapper_->create_reader();
    finish();
    wrapper_->reload();
}

template <typename T>
void
InvertedIndexTantivy<T>::BuildWithFieldData(
    const std::vector<std::shared_ptr<FieldDataBase>>& field_datas) {
    if (schema_.nullable()) {
        int64_t total = 0;
        for (const auto& data : field_datas) {
            total += data->get_null_count();
        }
        folly::SharedMutex::WriteHolder lock(mutex_);
        null_offset_.reserve(total);
    }
    switch (schema_.data_type()) {
        case proto::schema::DataType::Bool:
        case proto::schema::DataType::Int8:
        case proto::schema::DataType::Int16:
        case proto::schema::DataType::Int32:
        case proto::schema::DataType::Int64:
        case proto::schema::DataType::Float:
        case proto::schema::DataType::Double:
        case proto::schema::DataType::String:
        case proto::schema::DataType::VarChar: {
            // Generally, we will not build inverted index with single segment except for building index
            // for query node with older version(2.4). See more comments above `inverted_index_single_segment_`.
            if (!inverted_index_single_segment_) {
                int64_t offset = 0;
                if (schema_.nullable()) {
                    for (const auto& data : field_datas) {
                        auto n = data->get_num_rows();
                        for (int i = 0; i < n; i++) {
                            if (!data->is_valid(i)) {
                                folly::SharedMutex::WriteHolder lock(mutex_);
                                null_offset_.push_back(offset);
                            }
                            wrapper_->add_array_data<T>(
                                static_cast<const T*>(data->RawValue(i)),
                                data->is_valid(i),
                                offset++);
                        }
                    }
                } else {
                    for (const auto& data : field_datas) {
                        auto n = data->get_num_rows();
                        wrapper_->add_data<T>(
                            static_cast<const T*>(data->Data()), n, offset);
                        offset += n;
                    }
                }
            } else {
                for (const auto& data : field_datas) {
                    auto n = data->get_num_rows();
                    if (schema_.nullable()) {
                        for (int i = 0; i < n; i++) {
                            if (!data->is_valid(i)) {
                                folly::SharedMutex::WriteHolder lock(mutex_);
                                null_offset_.push_back(i);
                            }
                            wrapper_
                                ->add_array_data_by_single_segment_writer<T>(
                                    static_cast<const T*>(data->RawValue(i)),
                                    data->is_valid(i));
                        }
                        continue;
                    }
                    wrapper_->add_data_by_single_segment_writer<T>(
                        static_cast<const T*>(data->Data()), n);
                }
            }
            break;
        }

        case proto::schema::DataType::Array: {
            build_index_for_array(field_datas);
            break;
        }

        case proto::schema::DataType::JSON: {
            build_index_for_json(field_datas);
            break;
        }

        default:
            PanicInfo(ErrorCode::NotImplemented,
                      fmt::format("Inverted index not supported on {}",
                                  schema_.data_type()));
    }
}

template <typename T>
void
InvertedIndexTantivy<T>::build_index_for_array(
    const std::vector<std::shared_ptr<FieldDataBase>>& field_datas) {
    int64_t offset = 0;
    for (const auto& data : field_datas) {
        auto n = data->get_num_rows();
        auto array_column = static_cast<const Array*>(data->Data());
        for (int64_t i = 0; i < n; i++) {
            if (schema_.nullable() && !data->is_valid(i)) {
                folly::SharedMutex::WriteHolder lock(mutex_);
                null_offset_.push_back(offset);
            }
            auto length = data->is_valid(i) ? array_column[i].length() : 0;
            if (!inverted_index_single_segment_) {
                wrapper_->template add_array_data(
                    reinterpret_cast<const T*>(array_column[i].data()),
                    length,
                    offset++);
            } else {
                wrapper_->template add_array_data_by_single_segment_writer(
                    reinterpret_cast<const T*>(array_column[i].data()), length);
            }
        }
    }
}

template <>
void
InvertedIndexTantivy<std::string>::build_index_for_array(
    const std::vector<std::shared_ptr<FieldDataBase>>& field_datas) {
    int64_t offset = 0;
    for (const auto& data : field_datas) {
        auto n = data->get_num_rows();
        auto array_column = static_cast<const Array*>(data->Data());
        for (int64_t i = 0; i < n; i++) {
            Assert(IsStringDataType(array_column[i].get_element_type()));
            Assert(IsStringDataType(
                static_cast<DataType>(schema_.element_type())));
            if (schema_.nullable() && !data->is_valid(i)) {
                folly::SharedMutex::WriteHolder lock(mutex_);
                null_offset_.push_back(offset);
            }
            std::vector<std::string> output;
            for (int64_t j = 0; j < array_column[i].length(); j++) {
                output.push_back(
                    array_column[i].template get_data<std::string>(j));
            }
            auto length = data->is_valid(i) ? output.size() : 0;
            if (!inverted_index_single_segment_) {
                wrapper_->template add_array_data(
                    output.data(), length, offset++);
            } else {
                wrapper_->template add_array_data_by_single_segment_writer(
                    output.data(), length);
            }
        }
    }
}

template class InvertedIndexTantivy<bool>;
template class InvertedIndexTantivy<int8_t>;
template class InvertedIndexTantivy<int16_t>;
template class InvertedIndexTantivy<int32_t>;
template class InvertedIndexTantivy<int64_t>;
template class InvertedIndexTantivy<float>;
template class InvertedIndexTantivy<double>;
template class InvertedIndexTantivy<std::string>;
}  // namespace milvus::index
