/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "lock_manager.h"
#include <algorithm>
#include <stdexcept>

inline bool txn_can_take_lock(Transaction *txn) {
    auto state = txn->get_state();
    // 常见实现：DEFAULT/GROWING 可加锁；SHRINKING/COMMITTED/ABORTED 不可加锁
    return state == TransactionState::DEFAULT || state == TransactionState::GROWING;
}

inline void mark_txn_abort(Transaction *txn) {
    txn->set_state(TransactionState::ABORTED);
}

inline void ensure_txn_growing(Transaction *txn) {
    if (txn->get_state() == TransactionState::DEFAULT) {
        txn->set_state(TransactionState::GROWING);
    }
}


using LM = LockManager;
using LockMode = LockManager::LockMode;
using GroupLockMode = LockManager::GroupLockMode;
using LockRequest = LockManager::LockRequest;
using LockRequestQueue = LockManager::LockRequestQueue;

bool compatible_with_group(GroupLockMode g, LockMode req) {
    switch (req) {
        case LockMode::INTENTION_SHARED:
            return g == GroupLockMode::NON_LOCK ||
                   g == GroupLockMode::IS ||
                   g == GroupLockMode::IX ||
                   g == GroupLockMode::S ||
                   g == GroupLockMode::SIX;

        case LockMode::INTENTION_EXCLUSIVE:
            return g == GroupLockMode::NON_LOCK ||
                   g == GroupLockMode::IS ||
                   g == GroupLockMode::IX;

        case LockMode::SHARED:
            return g == GroupLockMode::NON_LOCK ||
                   g == GroupLockMode::IS ||
                   g == GroupLockMode::S;

        case LockMode::S_IX: // SIX
            return g == GroupLockMode::NON_LOCK ||
                   g == GroupLockMode::IS;

        case LockMode::EXLUCSIVE:
            return g == GroupLockMode::NON_LOCK;
    }
    return false;
}


bool pairwise_conflict(LockMode a, LockMode b) {
    // 返回 true 表示冲突
    auto compat = [](LockMode x, LockMode y) -> bool {
        if (x == LockMode::INTENTION_SHARED) {
            return y == LockMode::INTENTION_SHARED ||
                   y == LockMode::INTENTION_EXCLUSIVE ||
                   y == LockMode::SHARED ||
                   y == LockMode::S_IX;
        }
        if (x == LockMode::INTENTION_EXCLUSIVE) {
            return y == LockMode::INTENTION_SHARED ||
                   y == LockMode::INTENTION_EXCLUSIVE;
        }
        if (x == LockMode::SHARED) {
            return y == LockMode::INTENTION_SHARED ||
                   y == LockMode::SHARED;
        }
        if (x == LockMode::S_IX) {
            return y == LockMode::INTENTION_SHARED;
        }
        if (x == LockMode::EXLUCSIVE) {
            return false;
        }
        return false;
    };
    return !(compat(a, b) && compat(b, a));
}

// 找本事务在队列中的请求
std::list<LockRequest>::iterator find_req(LockRequestQueue *q, txn_id_t txn_id) {
    return std::find_if(q->request_queue_.begin(), q->request_queue_.end(),
                        [&](const LockRequest &r) { return r.txn_id_ == txn_id; });
}

// 升级兼容性：old -> target 是否是合法升级
bool valid_upgrade(LockMode old_mode, LockMode target_mode) {
    if (old_mode == target_mode) {
        return true;
    }

    // 记录锁
    if (old_mode == LockMode::SHARED && target_mode == LockMode::EXLUCSIVE) {
        return true;
    }

    // 表锁升级
    if (old_mode == LockMode::INTENTION_SHARED &&
        (target_mode == LockMode::INTENTION_EXCLUSIVE || target_mode == LockMode::SHARED ||
         target_mode == LockMode::S_IX || target_mode == LockMode::EXLUCSIVE)) {
        return true;
    }

    if (old_mode == LockMode::INTENTION_EXCLUSIVE &&
        (target_mode == LockMode::S_IX || target_mode == LockMode::EXLUCSIVE)) {
        return true;
    }

    if (old_mode == LockMode::SHARED &&
        (target_mode == LockMode::S_IX || target_mode == LockMode::EXLUCSIVE)) {
        return true;
    }

    if (old_mode == LockMode::S_IX && target_mode == LockMode::EXLUCSIVE) {
        return true;
    }

    return false;
}

bool compatible_ignoring_self(LockRequestQueue *q, txn_id_t txn_id, LockMode target) {
    for (auto &req : q->request_queue_) {
        if (!req.granted_ || req.txn_id_ == txn_id) {
            continue;
        }
        if (pairwise_conflict(req.lock_mode_, target)) {
            return false;
        }
    }
    return true;
}

bool LockManager::check_conflict(LockMode aim_mode, LockMode req_mode) {
    return pairwise_conflict(aim_mode, req_mode);
}

bool LockManager::deadlock_prevention_check(LockRequestQueue *request_queue, Transaction *txn, LockMode aim_mode) {
    // no-wait：只要和别的已授予锁冲突，直接失败并中止当前事务
    for (auto &req : request_queue->request_queue_) {
        if (!req.granted_) {
            continue;
        }
        if (req.txn_id_ == txn->get_transaction_id()) {
            continue;
        }
        if (pairwise_conflict(req.lock_mode_, aim_mode)) {
            mark_txn_abort(txn);
            return false;
        }
    }
    return true;
}

void LockManager::update_group_lock_mode(LockRequestQueue* request_queue) {
    int s_cnt = 0;
    int ix_cnt = 0;
    bool has_is = false;
    bool has_x = false;
    bool has_six = false;

    for (auto &req : request_queue->request_queue_) {
        if (!req.granted_) {
            continue;
        }
        switch (req.lock_mode_) {
            case LockMode::SHARED:
                ++s_cnt;
                break;
            case LockMode::INTENTION_EXCLUSIVE:
                ++ix_cnt;
                break;
            case LockMode::INTENTION_SHARED:
                has_is = true;
                break;
            case LockMode::S_IX:
                has_six = true;
                break;
            case LockMode::EXLUCSIVE:
                has_x = true;
                break;
        }
    }

    request_queue->shared_lock_num_ = s_cnt;
    request_queue->IX_lock_num_ = ix_cnt;

    if (has_x) {
        request_queue->group_lock_mode_ = GroupLockMode::X;
        return;
    }
    if (has_six) {
        request_queue->group_lock_mode_ = GroupLockMode::SIX;
        return;
    }
    if (s_cnt > 0 && ix_cnt > 0) {
        request_queue->group_lock_mode_ = GroupLockMode::SIX;
        return;
    }
    if (s_cnt > 0) {
        request_queue->group_lock_mode_ = GroupLockMode::S;
        return;
    }
    if (ix_cnt > 0) {
        request_queue->group_lock_mode_ = GroupLockMode::IX;
        return;
    }
    if (has_is) {
        request_queue->group_lock_mode_ = GroupLockMode::IS;
        return;
    }
    request_queue->group_lock_mode_ = GroupLockMode::NON_LOCK;
}

/**
 * @description: 申请行级共享锁
 * @return {bool} 加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {Rid&} rid 加锁的目标记录ID 记录所在的表的fd
 * @param {int} tab_fd
 */
bool LockManager::lock_shared_on_record(Transaction* txn, const Rid& rid, int tab_fd) {
    std::scoped_lock<std::mutex> lk(latch_);

    if (!txn_can_take_lock(txn)) {
        return false;
    }
    ensure_txn_growing(txn);

    // 先拿表 IS
    LockDataId table_id(tab_fd, LockDataType::TABLE);
    auto &tq = lock_table_[table_id];
    auto t_it = find_req(&tq, txn->get_transaction_id());
    if (t_it == tq.request_queue_.end()) {
        if (!deadlock_prevention_check(&tq, txn, LockMode::INTENTION_SHARED)) {
            return false;
        }
        tq.request_queue_.emplace_back(txn->get_transaction_id(), LockMode::INTENTION_SHARED);
        tq.request_queue_.back().granted_ = true;
        update_group_lock_mode(&tq);
        txn->get_lock_set()->insert(table_id);
    }

    // 再拿记录 S
    LockDataId rid_id(tab_fd, rid, LockDataType::RECORD);
    auto &q = lock_table_[rid_id];
    auto it = find_req(&q, txn->get_transaction_id());
    if (it != q.request_queue_.end()) {
        // 已经有锁
        if (it->granted_ && (it->lock_mode_ == LockMode::SHARED || it->lock_mode_ == LockMode::EXLUCSIVE)) {
            return true;
        }
        return false;
    }

    if (!deadlock_prevention_check(&q, txn, LockMode::SHARED)) {
        return false;
    }

    q.request_queue_.emplace_back(txn->get_transaction_id(), LockMode::SHARED);
    q.request_queue_.back().granted_ = true;
    q.shared_lock_num_++;
    update_group_lock_mode(&q);
    txn->get_lock_set()->insert(rid_id);

    return true;
}

/**
 * @description: 申请行级排他锁
 * @return {bool} 加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {Rid&} rid 加锁的目标记录ID
 * @param {int} tab_fd 记录所在的表的fd
 */
bool LockManager::lock_exclusive_on_record(Transaction* txn, const Rid& rid, int tab_fd) {
    std::scoped_lock<std::mutex> lk(latch_);

    if (!txn_can_take_lock(txn)) {
        return false;
    }
    ensure_txn_growing(txn);

    // 先拿表 IX
    LockDataId table_id(tab_fd, LockDataType::TABLE);
    auto &tq = lock_table_[table_id];
    auto t_it = find_req(&tq, txn->get_transaction_id());
    if (t_it == tq.request_queue_.end()) {
        if (!deadlock_prevention_check(&tq, txn, LockMode::INTENTION_EXCLUSIVE)) {
            return false;
        }
        tq.request_queue_.emplace_back(txn->get_transaction_id(), LockMode::INTENTION_EXCLUSIVE);
        tq.request_queue_.back().granted_ = true;
        tq.IX_lock_num_++;
        update_group_lock_mode(&tq);
        txn->get_lock_set()->insert(table_id);
    } else {
        // 表锁如果只有 IS，要升级到 IX
        if (t_it->lock_mode_ == LockMode::INTENTION_SHARED) {
            if (!compatible_ignoring_self(&tq, txn->get_transaction_id(), LockMode::INTENTION_EXCLUSIVE)) {
                mark_txn_abort(txn);
                return false;
            }
            t_it->lock_mode_ = LockMode::INTENTION_EXCLUSIVE;
            update_group_lock_mode(&tq);
        }
    }

    // 再拿记录 X
    LockDataId rid_id(tab_fd, rid, LockDataType::RECORD);
    auto &q = lock_table_[rid_id];
    auto it = find_req(&q, txn->get_transaction_id());

    if (it == q.request_queue_.end()) {
        if (!deadlock_prevention_check(&q, txn, LockMode::EXLUCSIVE)) {
            return false;
        }
        q.request_queue_.emplace_back(txn->get_transaction_id(), LockMode::EXLUCSIVE);
        q.request_queue_.back().granted_ = true;
        update_group_lock_mode(&q);
        txn->get_lock_set()->insert(rid_id);
        return true;
    }

    if (it->lock_mode_ == LockMode::EXLUCSIVE) {
        return true;
    }

    if (it->lock_mode_ == LockMode::SHARED) {
        if (!compatible_ignoring_self(&q, txn->get_transaction_id(), LockMode::EXLUCSIVE)) {
            mark_txn_abort(txn);
            return false;
        }
        it->lock_mode_ = LockMode::EXLUCSIVE;
        update_group_lock_mode(&q);
        return true;
    }

    return false;
}

/**
 * @description: 申请表级读锁
 * @return {bool} 返回加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {int} tab_fd 目标表的fd
 */
bool LockManager::lock_shared_on_table(Transaction* txn, int tab_fd) {
    
    return true;
}

/**
 * @description: 申请表级写锁
 * @return {bool} 返回加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {int} tab_fd 目标表的fd
 */
bool LockManager::lock_exclusive_on_table(Transaction* txn, int tab_fd) {
    
    return true;
}

/**
 * @description: 申请表级意向读锁
 * @return {bool} 返回加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {int} tab_fd 目标表的fd
 */
bool LockManager::lock_IS_on_table(Transaction* txn, int tab_fd) {
    
    return true;
}

/**
 * @description: 申请表级意向写锁
 * @return {bool} 返回加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {int} tab_fd 目标表的fd
 */
bool LockManager::lock_IX_on_table(Transaction* txn, int tab_fd) {
    
    return true;
}

/**
 * @description: 释放锁
 * @return {bool} 返回解锁是否成功
 * @param {Transaction*} txn 要释放锁的事务对象指针
 * @param {LockDataId} lock_data_id 要释放的锁ID
 */
bool LockManager::unlock(Transaction* txn, LockDataId lock_data_id) {
   
    return true;
}