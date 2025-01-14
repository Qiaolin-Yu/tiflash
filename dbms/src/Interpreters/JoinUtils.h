// Copyright 2023 PingCAP, Ltd.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <Columns/ColumnNullable.h>
#include <Core/Block.h>
#include <Parsers/ASTTablesInSelectQuery.h>

namespace DB
{
/// Do I need to use the hash table maps_*_full, in which we remember whether the row was joined.
inline bool getFullness(ASTTableJoin::Kind kind)
{
    return kind == ASTTableJoin::Kind::Right || kind == ASTTableJoin::Kind::Cross_Right || kind == ASTTableJoin::Kind::Full;
}
inline bool isLeftJoin(ASTTableJoin::Kind kind)
{
    return kind == ASTTableJoin::Kind::Left || kind == ASTTableJoin::Kind::Cross_Left;
}
inline bool isRightJoin(ASTTableJoin::Kind kind)
{
    return kind == ASTTableJoin::Kind::Right || kind == ASTTableJoin::Kind::Cross_Right;
}
inline bool isInnerJoin(ASTTableJoin::Kind kind)
{
    return kind == ASTTableJoin::Kind::Inner || kind == ASTTableJoin::Kind::Cross;
}
inline bool isAntiJoin(ASTTableJoin::Kind kind)
{
    return kind == ASTTableJoin::Kind::Anti || kind == ASTTableJoin::Kind::Cross_Anti;
}
inline bool isCrossJoin(ASTTableJoin::Kind kind)
{
    return kind == ASTTableJoin::Kind::Cross || kind == ASTTableJoin::Kind::Cross_Left
        || kind == ASTTableJoin::Kind::Cross_Right || kind == ASTTableJoin::Kind::Cross_Anti
        || kind == ASTTableJoin::Kind::Cross_LeftSemi || kind == ASTTableJoin::Kind::Cross_LeftAnti;
}
/// (cartesian/null-aware) (anti) left semi join.
inline bool isLeftSemiFamily(ASTTableJoin::Kind kind)
{
    return kind == ASTTableJoin::Kind::LeftSemi || kind == ASTTableJoin::Kind::LeftAnti
        || kind == ASTTableJoin::Kind::Cross_LeftSemi || kind == ASTTableJoin::Kind::Cross_LeftAnti
        || kind == ASTTableJoin::Kind::NullAware_LeftSemi || kind == ASTTableJoin::Kind::NullAware_LeftAnti;
}
inline bool isNullAwareSemiFamily(ASTTableJoin::Kind kind)
{
    return kind == ASTTableJoin::Kind::NullAware_Anti || kind == ASTTableJoin::Kind::NullAware_LeftAnti
        || kind == ASTTableJoin::Kind::NullAware_LeftSemi;
}

bool mayProbeSideExpandedAfterJoin(ASTTableJoin::Kind kind, ASTTableJoin::Strictness strictness);

struct ProbeProcessInfo
{
    Block block;
    size_t partition_index;
    UInt64 max_block_size;
    UInt64 min_result_block_size;
    size_t start_row;
    size_t end_row;
    bool all_rows_joined_finish;

    /// these are used for probe
    bool prepare_for_probe_done = false;
    Columns materialized_columns;
    ColumnRawPtrs key_columns;
    ColumnPtr null_map_holder = nullptr;
    ConstNullMapPtr null_map = nullptr;
    /// Used with ANY INNER JOIN
    std::unique_ptr<IColumn::Filter> filter = nullptr;
    /// Used with ALL ... JOIN
    std::unique_ptr<IColumn::Offsets> offsets_to_replicate = nullptr;

    explicit ProbeProcessInfo(UInt64 max_block_size_)
        : max_block_size(max_block_size_)
        , min_result_block_size((max_block_size + 1) / 2)
        , all_rows_joined_finish(true){};

    void resetBlock(Block && block_, size_t partition_index_ = 0);
    void updateStartRow();
    void prepareForProbe(const Names & key_names, const String & filter_column, ASTTableJoin::Kind kind, ASTTableJoin::Strictness strictness);
};
struct JoinBuildInfo
{
    bool enable_fine_grained_shuffle;
    size_t fine_grained_shuffle_count;
    bool enable_spill;
    bool is_spilled;
    size_t build_concurrency;
    size_t restore_round;
    bool needVirtualDispatchForProbeBlock() const
    {
        return enable_fine_grained_shuffle || (enable_spill && !is_spilled);
    }
};
void computeDispatchHash(size_t rows,
                         const ColumnRawPtrs & key_columns,
                         const TiDB::TiDBCollators & collators,
                         std::vector<String> & partition_key_containers,
                         size_t join_restore_round,
                         WeakHash32 & hash);
ColumnRawPtrs extractAndMaterializeKeyColumns(const Block & block, Columns & materialized_columns, const Strings & key_columns_names);
void recordFilteredRows(const Block & block, const String & filter_column, ColumnPtr & null_map_holder, ConstNullMapPtr & null_map);
} // namespace DB
