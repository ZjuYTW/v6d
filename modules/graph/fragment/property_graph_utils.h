/** Copyright 2020-2021 Alibaba Group Holding Limited.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#ifndef MODULES_GRAPH_FRAGMENT_PROPERTY_GRAPH_UTILS_H_
#define MODULES_GRAPH_FRAGMENT_PROPERTY_GRAPH_UTILS_H_

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "arrow/api.h"
#include "arrow/compute/api.h"
#include "arrow/io/api.h"
#include "arrow/ipc/api.h"
#include "boost/leaf.hpp"

#include "grape/serialization/in_archive.h"
#include "grape/serialization/out_archive.h"
#include "grape/utils/atomic_ops.h"
#include "grape/utils/vertex_array.h"
#include "grape/worker/comm_spec.h"

#include "graph/fragment/property_graph_types.h"
#include "graph/utils/error.h"
#include "graph/utils/mpi_utils.h"

#include "basic/ds/hashmap.h"

namespace vineyard {

inline boost::leaf::result<std::shared_ptr<arrow::Schema>> TypeLoosen(
    const std::vector<std::shared_ptr<arrow::Schema>>& schemas) {
  int field_num = -1;
  std::shared_ptr<arrow::KeyValueMetadata> metadata(
      new arrow::KeyValueMetadata());
  for (const auto& schema : schemas) {
    if (schema != nullptr) {
      if (field_num != -1 && field_num != schema->num_fields()) {
        RETURN_GS_ERROR(ErrorCode::kInvalidOperationError,
                        "Inconsistent field number");
      }
      field_num = schema->num_fields();
      if (schema->metadata() != nullptr) {
        std::unordered_map<std::string, std::string> metakv;
        schema->metadata()->ToUnorderedMap(&metakv);
        for (auto const& kv : metakv) {
          metadata->Append(kv.first, kv.second);
        }
      }
    }
  }
  if (field_num == -1) {
    RETURN_GS_ERROR(ErrorCode::kInvalidOperationError,
                    "No table available for normalizing schemas");
  }
  if (field_num == 0) {
    RETURN_GS_ERROR(ErrorCode::kInvalidOperationError, "Every schema is empty");
  }
  // Perform type lossen.
  // Date32 -> int32
  // Timestamp -> int64 -> double -> utf8   binary (not supported)

  // Timestamp value are stored as as number of seconds, milliseconds,
  // microseconds or nanoseconds since UNIX epoch.
  // CSV reader can only produce timestamp in seconds.
  std::vector<std::vector<std::shared_ptr<arrow::Field>>> fields(field_num);
  for (int i = 0; i < field_num; ++i) {
    for (const auto& schema : schemas) {
      if (schema != nullptr) {
        fields[i].push_back(schema->field(i));
      }
    }
  }
  std::vector<std::shared_ptr<arrow::Field>> lossen_fields(field_num);

  for (int i = 0; i < field_num; ++i) {
    lossen_fields[i] = fields[i][0];
    if (fields[i][0]->type() == arrow::null()) {
      continue;
    }
    auto res = fields[i][0]->type();
    if (res->Equals(arrow::date32())) {
      res = arrow::int32();
    }
    if (res->Equals(arrow::date64())) {
      res = arrow::int64();
    }
    if (res->id() == arrow::Type::TIMESTAMP) {
      res = arrow::int64();
    }
    if (res->Equals(arrow::int64())) {
      for (size_t j = 1; j < fields[i].size(); ++j) {
        if (fields[i][j]->type()->Equals(arrow::float64())) {
          res = arrow::float64();
        }
      }
    }
    if (res->Equals(arrow::float64())) {
      for (size_t j = 1; j < fields[i].size(); ++j) {
        if (fields[i][j]->type()->Equals(arrow::utf8())) {
          res = arrow::utf8();
        }
      }
    }
    if (res->Equals(arrow::utf8())) {
      res = arrow::large_utf8();
    }
    lossen_fields[i] = lossen_fields[i]->WithType(res);
  }
  return std::make_shared<arrow::Schema>(lossen_fields, metadata);
}

inline boost::leaf::result<std::shared_ptr<arrow::Array>> CastStringToBigString(
    const std::shared_ptr<arrow::Array>& in,
    const std::shared_ptr<arrow::DataType>& to_type) {
  auto array_data = in->data()->Copy();
  auto offset = array_data->buffers[1];
  using from_offset_type = typename arrow::StringArray::offset_type;
  using to_string_offset_type = typename arrow::LargeStringArray::offset_type;
  auto raw_value_offsets_ =
      offset == NULLPTR
          ? NULLPTR
          : reinterpret_cast<const from_offset_type*>(offset->data());
  std::vector<to_string_offset_type> to_offset(offset->size() /
                                               sizeof(from_offset_type));
  for (size_t i = 0; i < to_offset.size(); ++i) {
    to_offset[i] = raw_value_offsets_[i];
  }
  std::shared_ptr<arrow::Buffer> buffer;
  arrow::TypedBufferBuilder<to_string_offset_type> buffer_builder;
  ARROW_OK_OR_RAISE(buffer_builder.Append(to_offset.data(), to_offset.size()));
  ARROW_OK_OR_RAISE(buffer_builder.Finish(&buffer));
  array_data->type = to_type;
  array_data->buffers[1] = buffer;
  auto out = arrow::MakeArray(array_data);
  ARROW_OK_OR_RAISE(out->ValidateFull());
  return out;
}

inline boost::leaf::result<std::shared_ptr<arrow::Array>> CastNullToOthers(
    const std::shared_ptr<arrow::Array>& in,
    const std::shared_ptr<arrow::DataType>& to_type) {
  std::unique_ptr<arrow::ArrayBuilder> builder;
  ARROW_OK_OR_RAISE(
      arrow::MakeBuilder(arrow::default_memory_pool(), to_type, &builder));
  ARROW_OK_OR_RAISE(builder->AppendNulls(in->length()));
  std::shared_ptr<arrow::Array> out;
  ARROW_OK_OR_RAISE(builder->Finish(&out));
  ARROW_OK_OR_RAISE(out->ValidateFull());
  return out;
}

inline boost::leaf::result<std::shared_ptr<arrow::Array>> GeneralCast(
    const std::shared_ptr<arrow::Array>& in,
    const std::shared_ptr<arrow::DataType>& to_type) {
  std::shared_ptr<arrow::Array> out;
#if defined(ARROW_VERSION) && ARROW_VERSION < 1000000
  arrow::compute::FunctionContext ctx;
  ARROW_OK_OR_RAISE(arrow::compute::Cast(&ctx, *in, to_type, {}, &out));
#else
  CHECK_ARROW_ERROR_AND_ASSIGN(out, arrow::compute::Cast(*in, to_type));
#endif
  return out;
}

inline boost::leaf::result<std::shared_ptr<arrow::Table>> CastTableToSchema(
    const std::shared_ptr<arrow::Table>& table,
    const std::shared_ptr<arrow::Schema>& schema) {
  if (table->schema()->Equals(schema)) {
    return table;
  }
  CHECK_OR_RAISE(table->num_columns() == schema->num_fields());
  std::vector<std::shared_ptr<arrow::ChunkedArray>> new_columns;
  for (int64_t i = 0; i < table->num_columns(); ++i) {
    auto col = table->column(i);
    if (!table->field(i)->type()->Equals(schema->field(i)->type())) {
      auto from_type = table->field(i)->type();
      auto to_type = schema->field(i)->type();
      std::vector<std::shared_ptr<arrow::Array>> chunks;
      for (int64_t j = 0; j < col->num_chunks(); ++j) {
        auto array = col->chunk(j);
        if (arrow::compute::CanCast(*from_type, *to_type)) {
          BOOST_LEAF_AUTO(new_array, GeneralCast(array, to_type));
          chunks.push_back(new_array);
        } else if (from_type->Equals(arrow::utf8()) &&
                   to_type->Equals(arrow::large_utf8())) {
          BOOST_LEAF_AUTO(new_array, CastStringToBigString(array, to_type));
          chunks.push_back(new_array);
        } else if (from_type->Equals(arrow::null())) {
          BOOST_LEAF_AUTO(new_array, CastNullToOthers(array, to_type));
          chunks.push_back(new_array);
        } else {
          RETURN_GS_ERROR(ErrorCode::kDataTypeError,
                          "Unsupported cast: To type: " + to_type->ToString() +
                              "; Origin type: " + from_type->ToString());
        }
      }
      auto chunk_array = std::make_shared<arrow::ChunkedArray>(chunks, to_type);
      new_columns.push_back(chunk_array);
    } else {
      new_columns.push_back(col);
    }
  }
  return arrow::Table::Make(schema, new_columns);
}

// This method used when several workers is loading a file in parallel, each
// worker will read a chunk of the origin file into a arrow::Table.
// We may get different table schemas as some chunks may have zero rows
// or some chunks' data doesn't have any floating numbers, but others might
// have. We could use this method to gather their schemas, and find out most
// inclusive fields, construct a new schema and broadcast back. Note: We perform
// type loosen, date32 -> int32; timestamp(s) -> int64 -> double -> string (big
// string), and any type is prior to null.
inline boost::leaf::result<std::shared_ptr<arrow::Table>> SyncSchema(
    const std::shared_ptr<arrow::Table>& table,
    const grape::CommSpec& comm_spec) {
  std::shared_ptr<arrow::Schema> local_schema =
      table != nullptr ? table->schema() : nullptr;
  std::vector<std::shared_ptr<arrow::Schema>> schemas;

  GlobalAllGatherv(local_schema, schemas, comm_spec);
  BOOST_LEAF_AUTO(normalized_schema, TypeLoosen(schemas));

  if (table == nullptr) {
    std::shared_ptr<arrow::Table> table_out;
    VY_OK_OR_RAISE(
        vineyard::EmptyTableBuilder::Build(normalized_schema, table_out));
    return table_out;
  } else {
    return CastTableToSchema(table, normalized_schema);
  }
}

template <typename ITER_T, typename FUNC_T>
void parallel_for(const ITER_T& begin, const ITER_T& end, const FUNC_T& func,
                  int thread_num, size_t chunk = 0) {
  std::vector<std::thread> threads(thread_num);
  size_t num = end - begin;
  if (chunk == 0) {
    chunk = (num + thread_num - 1) / thread_num;
  }
  std::atomic<size_t> cur(0);
  for (int i = 0; i < thread_num; ++i) {
    threads[i] = std::thread([&]() {
      while (true) {
        size_t x = cur.fetch_add(chunk);
        if (x >= num) {
          break;
        }
        size_t y = std::min(x + chunk, num);
        ITER_T a = begin + x;
        ITER_T b = begin + y;
        while (a != b) {
          func(a);
          ++a;
        }
      }
    });
  }
  for (auto& thrd : threads) {
    thrd.join();
  }
}

inline void parallel_prefix_sum(const int* input, int64_t* output,
                                size_t length, int concurrency) {
  size_t bsize =
      std::max(static_cast<size_t>(1024),
               static_cast<size_t>((length + concurrency - 1) / concurrency));
  int thread_num = static_cast<int>((length + bsize - 1) / bsize);

  auto block_prefix = [&](int i) {
    size_t begin = std::min(static_cast<size_t>(i) * bsize, length);
    size_t end = std::min(begin + bsize, length);
    output[begin] = input[begin];
    for (++begin; begin < end; ++begin) {
      output[begin] = input[begin] + output[begin - 1];
    }
  };

  std::vector<std::thread> threads_prefix;
  for (int i = 0; i < thread_num; ++i) {
    threads_prefix.emplace_back(block_prefix, i);
  }
  for (auto& thrd : threads_prefix) {
    thrd.join();
  }

  std::vector<int64_t> block_sum(thread_num);
  {
    size_t end = std::min(bsize, length);
    block_sum[0] = output[end - 1];
  }
  for (int i = 1; i < thread_num; ++i) {
    size_t begin = std::min(static_cast<size_t>(i) * bsize + bsize, length);
    block_sum[i] = block_sum[i - 1] + output[begin - 1];
  }

  auto block_add = [&](int i) {
    size_t begin = std::min(static_cast<size_t>(i) * bsize, length);
    size_t end = std::min(begin + bsize, length);
    for (; begin < end; ++begin) {
      output[begin] = output[begin] + block_sum[i - 1];
    }
  };

  std::vector<std::thread> threads_sum;
  for (int i = 1; i < thread_num; ++i) {
    threads_sum.emplace_back(block_add, i);
  }
  for (auto& thrd : threads_sum) {
    thrd.join();
  }
}

template <typename VID_T>
void collect_outer_vertices(
    const IdParser<VID_T>& parser,
    const std::shared_ptr<
        typename vineyard::ConvertToArrowType<VID_T>::ArrayType>& gid_array,
    fid_t fid, std::vector<std::vector<VID_T>>& collected_ovgids) {
  const VID_T* arr = gid_array->raw_values();
  for (int64_t i = 0; i < gid_array->length(); ++i) {
    if (parser.GetFid(arr[i]) != fid) {
      collected_ovgids[parser.GetLabelId(arr[i])].push_back(arr[i]);
    }
  }
}

template <typename VID_T>
boost::leaf::result<void> generate_outer_vertices_map(
    std::vector<std::vector<VID_T>>& collected_ovgids,
    const std::vector<VID_T>& start_ids, int vertex_label_num,
    std::vector<ska::flat_hash_map<
        VID_T, VID_T, typename Hashmap<VID_T, VID_T>::KeyHash>>& ovg2l_maps,
    std::vector<std::shared_ptr<
        typename vineyard::ConvertToArrowType<VID_T>::ArrayType>>&
        ovgid_lists) {
  ovg2l_maps.resize(vertex_label_num);
  ovgid_lists.resize(vertex_label_num);
  for (int i = 0; i < vertex_label_num; ++i) {
    auto& cur_list = collected_ovgids[i];
    std::sort(cur_list.begin(), cur_list.end());

    auto& cur_map = ovg2l_maps[i];
    typename ConvertToArrowType<VID_T>::BuilderType vec_builder;
    VID_T cur_id = start_ids[i];
    if (!cur_list.empty()) {
      cur_map.emplace(cur_list[0], cur_id);
      ARROW_OK_OR_RAISE(vec_builder.Append(cur_list[0]));
      ++cur_id;
    }
    for (size_t k = 1; k < cur_list.size(); ++k) {
      if (cur_list[k] != cur_list[k - 1]) {
        cur_map.emplace(cur_list[k], cur_id);
        ARROW_OK_OR_RAISE(vec_builder.Append(cur_list[k]));
        ++cur_id;
      }
    }
    ARROW_OK_OR_RAISE(vec_builder.Finish(&ovgid_lists[i]));
  }
  return {};
}

template <typename VID_T>
boost::leaf::result<void> generate_local_id_list(
    IdParser<VID_T>& parser,
    const std::shared_ptr<
        typename vineyard::ConvertToArrowType<VID_T>::ArrayType>& gid_list,
    fid_t fid,
    std::vector<ska::flat_hash_map<VID_T, VID_T,
                                   typename Hashmap<VID_T, VID_T>::KeyHash>>
        ovg2l_maps,
    int concurrency,
    std::shared_ptr<typename vineyard::ConvertToArrowType<VID_T>::ArrayType>&
        lid_list) {
  typename ConvertToArrowType<VID_T>::BuilderType builder;
  const VID_T* vec = gid_list->raw_values();
  int64_t length = gid_list->length();

  if (concurrency == 1) {
    for (int64_t i = 0; i < length; ++i) {
      VID_T gid = vec[i];
      if (parser.GetFid(gid) == fid) {
        ARROW_OK_OR_RAISE(builder.Append(parser.GenerateId(
            0, parser.GetLabelId(gid), parser.GetOffset(gid))));
      } else {
        ARROW_OK_OR_RAISE(
            builder.Append(ovg2l_maps[parser.GetLabelId(gid)].at(gid)));
      }
    }
  } else {
    ARROW_OK_OR_RAISE(builder.Resize(length));
    parallel_for(
        static_cast<int64_t>(0), length,
        [&vec, &parser, fid, &ovg2l_maps, &builder](int64_t i) {
          VID_T gid = vec[i];
          if (parser.GetFid(gid) == fid) {
            builder[i] = parser.GenerateId(0, parser.GetLabelId(gid),
                                           parser.GetOffset(gid));
          } else {
            builder[i] = ovg2l_maps[parser.GetLabelId(gid)].at(gid);
          }
        },
        concurrency);
    ARROW_OK_OR_RAISE(builder.Advance(length));
  }
  ARROW_OK_OR_RAISE(builder.Finish(&lid_list));
  return {};
}

template <typename VID_T, typename EID_T>
void sort_edges_with_respect_to_vertex(
    vineyard::PodArrayBuilder<property_graph_utils::NbrUnit<VID_T, EID_T>>&
        builder,
    std::shared_ptr<arrow::Int64Array> offsets, VID_T tvnum, int concurrency) {
  using nbr_unit_t = property_graph_utils::NbrUnit<VID_T, EID_T>;

  const int64_t* offsets_ptr = offsets->raw_values();
  if (concurrency == 1) {
    for (VID_T i = 0; i < tvnum; ++i) {
      nbr_unit_t* begin = builder.MutablePointer(offsets_ptr[i]);
      nbr_unit_t* end = builder.MutablePointer(offsets_ptr[i + 1]);
      std::sort(begin, end, [](const nbr_unit_t& lhs, const nbr_unit_t& rhs) {
        return lhs.vid < rhs.vid;
      });
    }
  } else {
    parallel_for(
        static_cast<VID_T>(0), tvnum,
        [offsets_ptr, &builder](VID_T i) {
          nbr_unit_t* begin = builder.MutablePointer(offsets_ptr[i]);
          nbr_unit_t* end = builder.MutablePointer(offsets_ptr[i + 1]);
          std::sort(begin, end,
                    [](const nbr_unit_t& lhs, const nbr_unit_t& rhs) {
                      return lhs.vid < rhs.vid;
                    });
        },
        concurrency);
  }
}

template <typename VID_T, typename EID_T>
void check_is_multigraph(
    vineyard::PodArrayBuilder<property_graph_utils::NbrUnit<VID_T, EID_T>>&
        builder,
    std::shared_ptr<arrow::Int64Array> offsets, VID_T tvnum, int concurrency,
    bool& is_multigraph) {
  using nbr_unit_t = property_graph_utils::NbrUnit<VID_T, EID_T>;
  const int64_t* offsets_ptr = offsets->raw_values();
  if (concurrency == 1) {
    for (VID_T i = 0; i < tvnum; ++i) {
      nbr_unit_t* begin = builder.MutablePointer(offsets_ptr[i]);
      nbr_unit_t* end = builder.MutablePointer(offsets_ptr[i + 1]);
      nbr_unit_t* loc = std::adjacent_find(
          begin, end, [](const nbr_unit_t& lhs, const nbr_unit_t& rhs) {
            return lhs.vid == rhs.vid;
          });
      if (loc != end) {
        is_multigraph = true;
        break;
      }
    }
  } else {
    parallel_for(
        static_cast<VID_T>(0), tvnum,
        [offsets_ptr, &builder, &is_multigraph](VID_T i) {
          if (!is_multigraph) {
            nbr_unit_t* begin = builder.MutablePointer(offsets_ptr[i]);
            nbr_unit_t* end = builder.MutablePointer(offsets_ptr[i + 1]);
            nbr_unit_t* loc = std::adjacent_find(
                begin, end, [](const nbr_unit_t& lhs, const nbr_unit_t& rhs) {
                  return lhs.vid == rhs.vid;
                });
            if (loc != end) {
              __sync_or_and_fetch(
                  reinterpret_cast<unsigned char*>(&is_multigraph), 1);
            }
          }
        },
        concurrency);
  }
}

template <typename VID_T, typename EID_T>
boost::leaf::result<void> generate_directed_csr(
    IdParser<VID_T>& parser,
    const std::shared_ptr<
        typename vineyard::ConvertToArrowType<VID_T>::ArrayType>& src_list,
    const std::shared_ptr<
        typename vineyard::ConvertToArrowType<VID_T>::ArrayType>& dst_list,
    std::vector<VID_T> tvnums, int vertex_label_num, int concurrency,
    std::vector<std::shared_ptr<arrow::FixedSizeBinaryArray>>& edges,
    std::vector<std::shared_ptr<arrow::Int64Array>>& edge_offsets,
    bool& is_multigraph) {
  std::vector<std::vector<int>> degree(vertex_label_num);
  std::vector<int64_t> actual_edge_num(vertex_label_num, 0);
  for (int v_label = 0; v_label != vertex_label_num; ++v_label) {
    degree[v_label].resize(tvnums[v_label], 0);
  }
  int64_t edge_num = src_list->length();
  const VID_T* src_list_ptr = src_list->raw_values();
  const VID_T* dst_list_ptr = dst_list->raw_values();

  if (concurrency == 1) {
    for (int64_t i = 0; i < edge_num; ++i) {
      VID_T src_id = src_list_ptr[i];
      ++degree[parser.GetLabelId(src_id)][parser.GetOffset(src_id)];
    }
  } else {
    parallel_for(
        static_cast<int64_t>(0), edge_num,
        [&degree, parser, src_list_ptr](int64_t i) {
          VID_T src_id = src_list_ptr[i];
          auto label = parser.GetLabelId(src_id);
          int64_t offset = parser.GetOffset(src_id);
          grape::atomic_add(degree[label][offset], 1);
        },
        concurrency);
  }

  std::vector<std::vector<int64_t>> offsets(vertex_label_num);
  for (int v_label = 0; v_label != vertex_label_num; ++v_label) {
    auto tvnum = tvnums[v_label];
    auto& offset_vec = offsets[v_label];
    auto& degree_vec = degree[v_label];
    arrow::Int64Builder builder;

    offset_vec.resize(tvnum + 1);
    offset_vec[0] = 0;

    if (tvnum > 0) {
      if (concurrency == 1) {
        for (VID_T i = 0; i < tvnum; ++i) {
          offset_vec[i + 1] = offset_vec[i] + degree_vec[i];
        }
        ARROW_OK_OR_RAISE(builder.AppendValues(offset_vec));
      } else {
        parallel_prefix_sum(degree_vec.data(), &offset_vec[1], tvnum,
                            concurrency);
        ARROW_OK_OR_RAISE(builder.Resize(tvnum + 1));
        parallel_for(
            static_cast<VID_T>(0), tvnum + 1,
            [&offset_vec, &builder](VID_T i) { builder[i] = offset_vec[i]; },
            concurrency);
        ARROW_OK_OR_RAISE(builder.Advance(tvnum + 1));
      }
    } else {
      ARROW_OK_OR_RAISE(builder.AppendValues(offset_vec));
    }
    ARROW_OK_OR_RAISE(builder.Finish(&edge_offsets[v_label]));
    actual_edge_num[v_label] = offset_vec[tvnum];
  }
  using nbr_unit_t = property_graph_utils::NbrUnit<VID_T, EID_T>;
  std::vector<vineyard::PodArrayBuilder<nbr_unit_t>> edge_builders(
      vertex_label_num);
  for (int v_label = 0; v_label != vertex_label_num; ++v_label) {
    // FixedSizeBinaryBuilder has different behaviour on `Resize/Advance`
    ARROW_OK_OR_RAISE(
        edge_builders[v_label].ResizeAndFill(actual_edge_num[v_label]));
  }

  if (concurrency == 1) {
    for (int64_t i = 0; i < edge_num; ++i) {
      VID_T src_id = src_list_ptr[i];
      int v_label = parser.GetLabelId(src_id);
      int64_t v_offset = parser.GetOffset(src_id);
      nbr_unit_t* ptr =
          edge_builders[v_label].MutablePointer(offsets[v_label][v_offset]);
      ptr->vid = dst_list->Value(i);
      ptr->eid = static_cast<EID_T>(i);
      ++offsets[v_label][v_offset];
    }
  } else {
    parallel_for(
        static_cast<int64_t>(0), edge_num,
        [src_list_ptr, dst_list_ptr, &parser, &edge_builders,
         &offsets](int64_t i) {
          VID_T src_id = src_list_ptr[i];
          int v_label = parser.GetLabelId(src_id);
          int64_t v_offset = parser.GetOffset(src_id);
          int64_t adj_offset =
              __sync_fetch_and_add(&offsets[v_label][v_offset], 1);
          nbr_unit_t* ptr = edge_builders[v_label].MutablePointer(adj_offset);
          ptr->vid = dst_list_ptr[i];
          ptr->eid = static_cast<EID_T>(i);
        },
        concurrency);
  }

  for (int v_label = 0; v_label != vertex_label_num; ++v_label) {
    sort_edges_with_respect_to_vertex(edge_builders[v_label],
                                      edge_offsets[v_label], tvnums[v_label],
                                      concurrency);
    if (!is_multigraph) {
      check_is_multigraph(edge_builders[v_label], edge_offsets[v_label],
                          tvnums[v_label], concurrency, is_multigraph);
    }
    ARROW_OK_OR_RAISE(edge_builders[v_label].Advance(actual_edge_num[v_label]));
    ARROW_OK_OR_RAISE(edge_builders[v_label].Finish(&edges[v_label]));
  }
  return {};
}

template <typename VID_T, typename EID_T>
boost::leaf::result<void> generate_undirected_csr(
    IdParser<VID_T>& parser,
    const std::shared_ptr<
        typename vineyard::ConvertToArrowType<VID_T>::ArrayType>& src_list,
    const std::shared_ptr<
        typename vineyard::ConvertToArrowType<VID_T>::ArrayType>& dst_list,
    std::vector<VID_T> tvnums, int vertex_label_num, int concurrency,
    std::vector<std::shared_ptr<arrow::FixedSizeBinaryArray>>& edges,
    std::vector<std::shared_ptr<arrow::Int64Array>>& edge_offsets,
    bool& is_multigraph) {
  std::vector<std::vector<int>> degree(vertex_label_num);
  std::vector<int64_t> actual_edge_num(vertex_label_num, 0);
  for (int v_label = 0; v_label != vertex_label_num; ++v_label) {
    degree[v_label].resize(tvnums[v_label], 0);
  }
  int64_t edge_num = src_list->length();
  const VID_T* src_list_ptr = src_list->raw_values();
  const VID_T* dst_list_ptr = dst_list->raw_values();

  if (concurrency == 1) {
    for (int64_t i = 0; i < edge_num; ++i) {
      VID_T src_id = src_list_ptr[i];
      VID_T dst_id = dst_list_ptr[i];
      ++degree[parser.GetLabelId(src_id)][parser.GetOffset(src_id)];
      ++degree[parser.GetLabelId(dst_id)][parser.GetOffset(dst_id)];
    }
  } else {
    parallel_for(
        static_cast<int64_t>(0), edge_num,
        [&degree, &parser, src_list_ptr, dst_list_ptr](int64_t i) {
          auto src_id = src_list_ptr[i];
          auto dst_id = dst_list_ptr[i];
          grape::atomic_add(
              degree[parser.GetLabelId(src_id)][parser.GetOffset(src_id)], 1);
          grape::atomic_add(
              degree[parser.GetLabelId(dst_id)][parser.GetOffset(dst_id)], 1);
        },
        concurrency);
  }

  std::vector<std::vector<int64_t>> offsets(vertex_label_num);
  for (int v_label = 0; v_label != vertex_label_num; ++v_label) {
    auto tvnum = tvnums[v_label];
    auto& offset_vec = offsets[v_label];
    auto& degree_vec = degree[v_label];
    arrow::Int64Builder builder;

    offset_vec.resize(tvnum + 1);
    offset_vec[0] = 0;

    if (tvnum > 0) {
      if (concurrency == 1) {
        for (VID_T i = 0; i < tvnum; ++i) {
          offset_vec[i + 1] = offset_vec[i] + degree_vec[i];
        }
        ARROW_OK_OR_RAISE(builder.AppendValues(offset_vec));
      } else {
        parallel_prefix_sum(degree_vec.data(), &offset_vec[1], tvnum,
                            concurrency);
        ARROW_OK_OR_RAISE(builder.Resize(tvnum + 1));
        parallel_for(
            static_cast<VID_T>(0), tvnum + 1,
            [&offset_vec, &builder](VID_T i) { builder[i] = offset_vec[i]; },
            concurrency);
        ARROW_OK_OR_RAISE(builder.Advance(tvnum + 1));
      }
    } else {
      ARROW_OK_OR_RAISE(builder.AppendValues(offset_vec));
    }
    ARROW_OK_OR_RAISE(builder.Finish(&edge_offsets[v_label]));
    actual_edge_num[v_label] = offset_vec[tvnum];
  }

  using nbr_unit_t = property_graph_utils::NbrUnit<VID_T, EID_T>;

  std::vector<vineyard::PodArrayBuilder<nbr_unit_t>> edge_builders(
      vertex_label_num);
  for (int v_label = 0; v_label != vertex_label_num; ++v_label) {
    // FixedSizeBinaryBuilder has different behaviour on `Resize/Advance`
    ARROW_OK_OR_RAISE(
        edge_builders[v_label].ResizeAndFill(actual_edge_num[v_label]));
  }

  if (concurrency == 1) {
    for (int64_t i = 0; i < edge_num; ++i) {
      VID_T src_id = src_list_ptr[i];
      VID_T dst_id = dst_list_ptr[i];
      auto src_label = parser.GetLabelId(src_id);
      int64_t src_offset = parser.GetOffset(src_id);
      auto dst_label = parser.GetLabelId(dst_id);
      int64_t dst_offset = parser.GetOffset(dst_id);

      nbr_unit_t* src_ptr = edge_builders[src_label].MutablePointer(
          offsets[src_label][src_offset]);
      src_ptr->vid = dst_id;
      src_ptr->eid = static_cast<EID_T>(i);
      ++offsets[src_label][src_offset];

      nbr_unit_t* dst_ptr = edge_builders[dst_label].MutablePointer(
          offsets[dst_label][dst_offset]);
      dst_ptr->vid = src_id;
      dst_ptr->eid = static_cast<EID_T>(i);
      ++offsets[dst_label][dst_offset];
    }
  } else {
    parallel_for(
        static_cast<int64_t>(0), edge_num,
        [src_list_ptr, dst_list_ptr, &parser, &edge_builders,
         &offsets](int64_t i) {
          VID_T src_id = src_list_ptr[i];
          VID_T dst_id = dst_list_ptr[i];
          auto src_label = parser.GetLabelId(src_id);
          int64_t src_offset = parser.GetOffset(src_id);
          auto dst_label = parser.GetLabelId(dst_id);
          int64_t dst_offset = parser.GetOffset(dst_id);

          int64_t oe_offset =
              __sync_fetch_and_add(&offsets[src_label][src_offset], 1);
          nbr_unit_t* src_ptr =
              edge_builders[src_label].MutablePointer(oe_offset);
          src_ptr->vid = dst_id;
          src_ptr->eid = static_cast<EID_T>(i);

          int64_t ie_offset =
              __sync_fetch_and_add(&offsets[dst_label][dst_offset], 1);
          nbr_unit_t* dst_ptr =
              edge_builders[dst_label].MutablePointer(ie_offset);
          dst_ptr->vid = src_id;
          dst_ptr->eid = static_cast<EID_T>(i);
        },
        concurrency);
  }

  for (int v_label = 0; v_label != vertex_label_num; ++v_label) {
    sort_edges_with_respect_to_vertex(edge_builders[v_label],
                                      edge_offsets[v_label], tvnums[v_label],
                                      concurrency);
    if (!is_multigraph) {
      check_is_multigraph(edge_builders[v_label], edge_offsets[v_label],
                          tvnums[v_label], concurrency, is_multigraph);
    }
    ARROW_OK_OR_RAISE(edge_builders[v_label].Advance(actual_edge_num[v_label]));
    ARROW_OK_OR_RAISE(edge_builders[v_label].Finish(&edges[v_label]));
  }
  return {};
}

}  // namespace vineyard

namespace grape {
inline InArchive& operator<<(InArchive& in_archive,
                             std::shared_ptr<arrow::Schema>& schema) {
  if (schema != nullptr) {
    std::shared_ptr<arrow::Buffer> out;
#if defined(ARROW_VERSION) && ARROW_VERSION < 2000000
    CHECK_ARROW_ERROR_AND_ASSIGN(
        out, arrow::ipc::SerializeSchema(*schema, nullptr,
                                         arrow::default_memory_pool()));
#else
    CHECK_ARROW_ERROR_AND_ASSIGN(
        out,
        arrow::ipc::SerializeSchema(*schema, arrow::default_memory_pool()));
#endif
    in_archive.AddBytes(out->data(), out->size());
  }
  return in_archive;
}

inline OutArchive& operator>>(OutArchive& out_archive,
                              std::shared_ptr<arrow::Schema>& schema) {
  if (!out_archive.Empty()) {
    auto buffer = std::make_shared<arrow::Buffer>(
        reinterpret_cast<const uint8_t*>(out_archive.GetBuffer()),
        out_archive.GetSize());
    arrow::io::BufferReader reader(buffer);
    CHECK_ARROW_ERROR_AND_ASSIGN(schema,
                                 arrow::ipc::ReadSchema(&reader, nullptr));
  }
  return out_archive;
}
}  // namespace grape

#endif  // MODULES_GRAPH_FRAGMENT_PROPERTY_GRAPH_UTILS_H_
