/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "transaction_manager.h"
#include "record/rm_file_handle.h"
#include "system/sm_manager.h"

std::unordered_map<txn_id_t, Transaction *> TransactionManager::txn_map = {};

/**
 * @description: 事务的开始方法
 * @return {Transaction*} 开始事务的指针
 * @param {Transaction*} txn 事务指针，空指针代表需要创建新事务，否则开始已有事务
 * @param {LogManager*} log_manager 日志管理器指针
 */
Transaction * TransactionManager::begin(Transaction* txn, LogManager* log_manager) {
    // Todo:
    // 1. 判断传入事务参数是否为空指针
    // 2. 如果为空指针，创建新事务
    // 3. 把开始事务加入到全局事务表中
    // 4. 返回当前事务指针
    std::scoped_lock<std::mutex> lock(latch_);
    if(txn == nullptr) {
        txn_id_t txn_id = next_txn_id_++;
        Transaction* new_txn = new Transaction(txn_id);
        new_txn->set_start_ts(next_timestamp_++);
        TransactionManager::txn_map[txn_id] = new_txn;
        log_manager->add_log_to_buffer(new BeginLogRecord(txn_id));
        return new_txn;
    } else {
        assert(TransactionManager::txn_map.find(txn->get_transaction_id()) == TransactionManager::txn_map.end());
        TransactionManager::txn_map[txn->get_transaction_id()] = txn;
        log_manager->add_log_to_buffer(new BeginLogRecord(txn->get_transaction_id()));
        return txn;
    }
    
    return nullptr;
}

/**
 * @description: 事务的提交方法
 * @param {Transaction*} txn 需要提交的事务
 * @param {LogManager*} log_manager 日志管理器指针
 */
void TransactionManager::commit(Transaction* txn, LogManager* log_manager) {
    // Todo:
    // 1. 如果存在未提交的写操作，提交所有的写操作
    log_manager->add_log_to_buffer(new CommitLogRecord(txn->get_transaction_id()));

    // 4. 把事务日志刷入磁盘中
    log_manager->flush_log_to_disk();

    // 5. 更新事务状态
    txn->set_state(TransactionState::COMMITTED);
    // 2. 释放所有锁
    for (auto& lock : *(txn->get_lock_set())) {
        assert(lock_manager_->unlock(txn,lock));
    }

    // 3. 释放事务相关资源，eg.锁集
    txn->get_lock_set()->clear();
}

/**
 * @description: 事务的终止（回滚）方法
 * @param {Transaction *} txn 需要回滚的事务
 * @param {LogManager} *log_manager 日志管理器指针
 */
void TransactionManager::abort(Transaction * txn, LogManager *log_manager) {
    // Todo:
    // 1. 回滚所有写操作
    for (auto it = txn->get_write_set()->rbegin(); it != txn->get_write_set()->rend(); ++it) {
        WriteRecord *write_record = *it;
        RmFileHandle *file_handle = sm_manager_->fhs_[write_record->GetTableName()].get();
        switch (write_record->GetWriteType()) {
            case WType::INSERT_TUPLE: {
                file_handle->delete_record(write_record->GetRid(), nullptr);
                break;
            }
            case WType::DELETE_TUPLE: {
                file_handle->insert_record(write_record->GetRid(), write_record->GetRecord().data);
                break;
            }
            case WType::UPDATE_TUPLE: {
                file_handle->update_record(write_record->GetRid(), write_record->GetRecord().data, nullptr);
                break;
            }
             default:
                throw InternalError("Unexpected write type");
                break; 
         }
    }
    log_manager->add_log_to_buffer(new AbortLogRecord(txn->get_transaction_id()));
    // 4. 把事务日志刷入磁盘中
    log_manager->flush_log_to_disk();
    // 5. 更新事务状态
    txn->set_state(TransactionState::ABORTED);

    // 2. 释放所有锁
    for (auto& lock : *(txn->get_lock_set())) {
        assert(lock_manager_->unlock(txn,lock));
    }
    // 3. 清空事务相关资源，eg.锁集
    txn->get_lock_set()->clear();

}