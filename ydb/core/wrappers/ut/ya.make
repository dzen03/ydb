UNITTEST_FOR(ydb/core/wrappers)

FORK_SUBTESTS()

SPLIT_FACTOR(20)

IF (NOT OS_WINDOWS)
    PEERDIR(
        ydb/library/actors/core
        library/cpp/digest/md5
        library/cpp/testing/unittest
        contrib/libs/aws-sdk-cpp/aws-cpp-sdk-core
        ydb/core/protos
        ydb/core/testlib/basics/default
        yql/essentials/minikql/comp_nodes/llvm16
        ydb/core/wrappers/ut_helpers
    )
    SRCS(
        s3_wrapper_ut.cpp
    )
ENDIF()

YQL_LAST_ABI_VERSION()

END()
