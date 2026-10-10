/*
 * 单元测试统一入口：每个测试模块暴露 xxx_run() 并返回失败数。
 */

#include <stdio.h>

int smoke_run(void);
int media_run(void);
int ftl_run(void);
int ftl_s3_run(void);
int sched_tests_run(void);
int spor_tests_run(void);
int s6_tests_run(void);

int main(void)
{
    int failed = 0;

    printf("================ ssd-sim unit tests ================\n");

    failed += smoke_run();
    failed += media_run();
    failed += ftl_run();
    failed += ftl_s3_run();
    failed += sched_tests_run();
    failed += spor_tests_run();
    failed += s6_tests_run();

    printf("====================================================\n");
    if (failed != 0) {
        printf("FAILED: %d checks\n", failed);
        return 1;
    }
    printf("ALL PASSED\n");
    return 0;
}
