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

#ifndef SRC_CLIENT_DS_LRU_H_
#define SRC_CLIENT_DS_LRU_H_

#include <list>
#include "client/ds/blob.h"


namespace vineyard {
  class LRU {
    using lru_iter = std::list<ObjectID>::iterator;
    using clru_iter = std::list<ObjectID>::const_iterator;

    public:
    static LRU* GetInstance();

    void Emplace(ObjectID obj_id) const;

    void Erase(ObjectID obj_id);

    clru_iter begin() const

    private:
    LRU() {} // NOLINT

    std::list<ObjectID> cache_;
    std::unordered_map<ObjectID, lru_iter> iter_table_;
    size_t max_size_;

    static LRU* lru_;
  }
}