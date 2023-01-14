#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <unity.h>

#define UNITY_FREERTOS_PRIORITY 5
#define UNITY_FREERTOS_CPU 0
#define UNITY_FREERTOS_STACK_SIZE 8912

void app_main(void)
{
    while (true) {
        int c = getc(stdin);

        if (c == '\n') {
            UNITY_BEGIN();
            unity_run_all_tests();
            UNITY_END();
        }

        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}