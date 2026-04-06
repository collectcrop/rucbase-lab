/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class NestedLoopJoinExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> left_;    // 左儿子节点（需要join的表）
    std::unique_ptr<AbstractExecutor> right_;   // 右儿子节点（需要join的表）
    size_t len_;                                // join后获得的每条记录的长度
    std::vector<ColMeta> cols_;                 // join后获得的记录的字段

    std::vector<Condition> fed_conds_;          // join条件
    bool isend;

   public:
    NestedLoopJoinExecutor(std::unique_ptr<AbstractExecutor> left, std::unique_ptr<AbstractExecutor> right, 
                            std::vector<Condition> conds) {
        left_ = std::move(left);
        right_ = std::move(right);
        len_ = left_->tupleLen() + right_->tupleLen();
        cols_ = left_->cols();
        auto right_cols = right_->cols();
        for (auto &col : right_cols) {
            col.offset += left_->tupleLen();
        }

        cols_.insert(cols_.end(), right_cols.begin(), right_cols.end());
        isend = false;
        fed_conds_ = std::move(conds);

    }
    const std::vector<ColMeta> &cols() const override { return cols_; }
    bool is_end() const override {
        return isend;
    }
    
    size_t tupleLen() const override { return len_; }

    void advance_to_next_match() {
        while (!left_->is_end()) {
            while (!right_->is_end()) {
                if (match_conditions(left_->Next(), right_->Next())) {
                    return;
                }
                right_->nextTuple();
            }
            left_->nextTuple();
            if (left_->is_end()) break;
            right_->beginTuple();
        }
        isend = true;
    }

    bool match_conditions(const std::unique_ptr<RmRecord> &left_record, const std::unique_ptr<RmRecord> &right_record) {
        for (auto &cond : fed_conds_) {
            auto left_col_pos = get_col(left_->cols(), cond.lhs_col);
            const char* left_data = left_record->data + left_col_pos->offset;
            const char* right_data;
            int right_col_len = 0;
            if (cond.is_rhs_val) {
                right_data = cond.rhs_val.raw->data;
                right_col_len = (left_col_pos->type == TYPE_STRING) ? cond.rhs_val.str_val.size() : left_col_pos->len;
            } else {
                auto right_col_pos = get_col(right_->cols(), cond.is_rhs_val ? TabCol() : cond.rhs_col);
                right_data = right_record->data + right_col_pos->offset;
                right_col_len = right_col_pos->len;
            }
            
            if (!compare(left_data, right_data, left_col_pos->type, cond.op, left_col_pos->len, right_col_len)) {
                return false;
            }
        }
        return true;
    }

    void beginTuple() override {
        left_->beginTuple();
        if (left_->is_end()) { isend = true; return; }
        right_->beginTuple();
        advance_to_next_match();
    }

    void nextTuple() override {
        if (isend) return;
        right_->nextTuple();
        advance_to_next_match();
    }

    std::unique_ptr<RmRecord> Next() override {
        if (isend) return nullptr;
        std::unique_ptr<RmRecord> res = std::make_unique<RmRecord>(len_);
        memcpy(res->data, left_->Next()->data, left_->tupleLen());
        memcpy(res->data + left_->tupleLen(), right_->Next()->data, right_->tupleLen());
        _abstract_rid = left_->rid();
        return res;
    }

    Rid &rid() override { return _abstract_rid; }
};