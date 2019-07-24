/*******************************************************************************
 * Copyright 上海赜睿信息科技有限公司(Zilliz) - All Rights Reserved
 * Unauthorized copying of this file, via any medium is strictly prohibited.
 * Proprietary and confidential.
 ******************************************************************************/
#include "Meta.h"

#include <ctime>
#include <stdio.h>

namespace zilliz {
namespace milvus {
namespace engine {
namespace meta {

Meta::~Meta() = default;

DateT Meta::GetDate(const std::time_t& t, int day_delta) {
    struct tm ltm;
    localtime_r(&t, &ltm);
    if (day_delta > 0) {
        do {
            ++ltm.tm_mday;
            --day_delta;
        } while(day_delta > 0);
        mktime(&ltm);
    } else if (day_delta < 0) {
        do {
            --ltm.tm_mday;
            ++day_delta;
        } while(day_delta < 0);
        mktime(&ltm);
    } else {
        ltm.tm_mday;
    }
    return ltm.tm_year*10000 + ltm.tm_mon*100 + ltm.tm_mday;
}

DateT Meta::GetDateWithDelta(int day_delta) {
    return GetDate(std::time(nullptr), day_delta);
}

DateT Meta::GetDate() {
    return GetDate(std::time(nullptr), 0);
}

} // namespace meta
} // namespace engine
} // namespace milvus
} // namespace zilliz
