/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "rm_scan.h"
#include "rm_file_handle.h"

/**
 * @brief 初始化file_handle和rid
 * @param file_handle
 */
RmScan::RmScan(const RmFileHandle *file_handle) : file_handle_(file_handle) {
    // Todo:
    // 初始化file_handle和rid（指向第一个存放了记录的位置）
    for (int page_no = RM_FIRST_RECORD_PAGE;
        page_no < file_handle_->file_hdr_.num_pages;
        page_no++) {
        RmPageHandle page_handle = file_handle_->fetch_page_handle(page_no);
        if (page_handle.page_hdr->num_records == 0) continue;
        int slot_no = Bitmap::first_bit(true, page_handle.bitmap, file_handle_->file_hdr_.num_records_per_page);
        if (slot_no < file_handle_->file_hdr_.num_records_per_page) {
            rid_ = Rid{page_no, slot_no};
            return;
        }
    }
    rid_ = Rid{RM_NO_PAGE, -1};
}

/**
 * @brief 找到文件中下一个存放了记录的位置
 */
void RmScan::next() {
    // Todo:
    // 找到文件中下一个存放了记录的非空闲位置，用rid_来指向这个位置
    if (is_end()) return;

    int page_no = rid_.page_no;
    int slot_no = rid_.slot_no;
    int max_slot = file_handle_->file_hdr_.num_records_per_page;

    RmPageHandle page_handle = file_handle_->fetch_page_handle(page_no);

    int next_slot = Bitmap::next_bit(true, page_handle.bitmap, max_slot, slot_no);
    if (next_slot < max_slot) {
        rid_ = Rid{page_no, next_slot};
        return;
    }

    for (int next_page = page_no + 1; next_page < file_handle_->file_hdr_.num_pages; next_page++) {
        RmPageHandle next_handle = file_handle_->fetch_page_handle(next_page);
        if (next_handle.page_hdr->num_records == 0) continue;
        int first_slot = Bitmap::first_bit(true, next_handle.bitmap, max_slot);
        if (first_slot < max_slot) {
            rid_ = Rid{next_page, first_slot};
            return;
        }
    }

    rid_ = Rid{RM_NO_PAGE, -1};
}

/**
 * @brief ​ 判断是否到达文件末尾
 */
bool RmScan::is_end() const {
    // Todo: 修改返回值
    return rid_.page_no == RM_NO_PAGE;
}

/**
 * @brief RmScan内部存放的rid
 */
Rid RmScan::rid() const {
    return rid_;
}