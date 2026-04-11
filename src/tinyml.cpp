#include "tinyml.h"
#include <math.h>
#include <time.h>
#include "model_data.h"
#include "app_time_utils.h"

// 1. THÔNG SỐ CHUẨN HÓA (Đã khớp với 8-features từ Colab)
const float means[] = {27.85, 73.95, -0.0017, 0.0058, -0.0041, -0.0038, -0.0014, 0.0005};
const float scales[] = {3.41, 15.68, 2.56, 10.18, 0.71, 0.71, 0.71, 0.71};

constexpr int32_t kGmtOffsetSec = 7 * 3600;
constexpr int32_t kDaylightOffsetSec = 0;

namespace
{
    tflite::ErrorReporter *error_reporter = nullptr;
    const tflite::Model *model = nullptr;
    tflite::MicroInterpreter *interpreter = nullptr;
    TfLiteTensor *input = nullptr;
    TfLiteTensor *output = nullptr;
    
    constexpr int kTensorArenaSize = 16 * 1024; 
    uint8_t tensor_arena[kTensorArenaSize];

    // Cửa sổ trượt để làm mịn dữ liệu và tính toán sự biến thiên (Diff)
    constexpr int kAvgWindow = 5;      
    constexpr int kHistorySize = 30;   

    float temp_window[kAvgWindow]{};
    float hum_window[kAvgWindow]{};
    float temp_history[kHistorySize]{};
    float hum_history[kHistorySize]{};

    int window_pos = 0;
    int window_count = 0;
    int history_pos = 0;
    bool history_ready = false;
}

void setupTinyML()
{
    static tflite::MicroErrorReporter micro_error_reporter;
    error_reporter = &micro_error_reporter;

    model = tflite::GetModel(weather_model_int8_tflite); 
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        error_reporter->Report("Schema mismatch!");
        return;
    }

    static tflite::AllOpsResolver resolver;
    static tflite::MicroInterpreter static_interpreter(
        model, resolver, tensor_arena, kTensorArenaSize, error_reporter);
    interpreter = &static_interpreter;

    if (interpreter->AllocateTensors() != kTfLiteOk) return;

    input = interpreter->input(0);
    output = interpreter->output(0);
    Serial.println("[TinyML] Engine Ready with 8 Features.");
}

// ---- Tuning macros ----
#define TINYML_BATCH_SAMPLES   10   // N mau moi batch
#define TINYML_FALLBACK_HOUR   14
#define TINYML_FALLBACK_MONTH  4

void tiny_ml_task(void *pvParameters)
{
    app_context_t *ctx = static_cast<app_context_t *>(pvParameters);
    setupTinyML();

    if (ctx == nullptr || ctx->tinyMLQueue == nullptr || ctx->tinyResultQueue == nullptr ||
        interpreter == nullptr || input == nullptr || output == nullptr) {
        Serial.println("[TinyML] Invalid context or interpreter not ready.");
        vTaskDelete(nullptr);
        return;
    }

    sensor_data_t data{};

    // Batch accumulator
    uint16_t batchCount = 0;
    float sumTemp = 0.0f;
    float sumHum = 0.0f;
    TickType_t lastSensorTs = 0;

    // Previous batch average (T/H cu)
    bool hasPrevBatchAvg = false;
    float prevBatchTempAvg = 0.0f;
    float prevBatchHumAvg = 0.0f;

    while (1) {
        // Sensor task push vao queue -> TinyML task duoc danh thuc
        if (xQueueReceive(ctx->tinyMLQueue, &data, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (isnan(data.temperature) || isnan(data.humidity)) {
            continue;
        }

        // 1) Gom batch
        sumTemp += data.temperature;
        sumHum += data.humidity;
        batchCount++;
        lastSensorTs = data.timestamp;

        // 2) Chua du N mau -> chua infer
        if (batchCount < TINYML_BATCH_SAMPLES) {
            continue;
        }

        // 3) Du N mau -> tinh trung binh batch hien tai
        const float tempAvg = sumTemp / (float)batchCount;
        const float humAvg  = sumHum  / (float)batchCount;

        // Diff so voi trung binh batch truoc
        const float tDiff = hasPrevBatchAvg ? (tempAvg - prevBatchTempAvg) : 0.0f;
        const float hDiff = hasPrevBatchAvg ? (humAvg - prevBatchHumAvg) : 0.0f;

        // Lay time neu co sync, neu khong thi fallback
        int hour = TINYML_FALLBACK_HOUR;
        int month = TINYML_FALLBACK_MONTH;

        AppDateTime nowTime{};
        const bool hasNowTime = appTimeNow(kGmtOffsetSec, kDaylightOffsetSec, nowTime);
        if (hasNowTime) {
            hour = nowTime.hour;
            month = nowTime.month;
        }

        // 8 features
        const float m_sin = sin(2.0f * M_PI * month / 12.0f);
        const float m_cos = cos(2.0f * M_PI * month / 12.0f);
        const float h_sin = sin(2.0f * M_PI * hour / 24.0f);
        const float h_cos = cos(2.0f * M_PI * hour / 24.0f);

        float rawInputs[8] = {tempAvg, humAvg, tDiff, hDiff, m_sin, m_cos, h_sin, h_cos};

        // Quantize input
        const float inputScale = input->params.scale;
        const int inputZeroPoint = input->params.zero_point;

        for (int i = 0; i < 8; i++) {
            const float normalized = (rawInputs[i] - means[i]) / scales[i];
            input->data.int8[i] = (int8_t)(normalized / inputScale + inputZeroPoint);
        }

        // 4) Infer 1 lan cho moi batch N mau
        if (interpreter->Invoke() == kTfLiteOk) {
            const float outScale = output->params.scale;
            const int outZeroPoint = output->params.zero_point;
            const float probability = (output->data.int8[0] - outZeroPoint) * outScale;
            const bool isRain = (probability > 0.5f);

            tinyml_result_t result{};
            result.rainProbability = probability;
            result.isRain = isRain;
            result.sensorTimestamp = lastSensorTs;
            result.inferTimestamp = xTaskGetTickCount();
            xQueueOverwrite(ctx->tinyResultQueue, &result);

            if (hasNowTime) {
                char iso[24];
                appTimeFormatIso(nowTime, iso, sizeof(iso));
                Serial.printf("%s | Tavg:%.2f Havg:%.2f | dT:%.2f dH:%.2f | Rain: %.1f%% -> %s\n",
                    iso, tempAvg, humAvg, tDiff, hDiff,
                    probability * 100.0f, isRain ? "RAIN" : "SUNNY");
            } else {
                Serial.printf("TIME_NA | Tavg:%.2f Havg:%.2f | dT:%.2f dH:%.2f | Rain: %.1f%% -> %s\n",
                    tempAvg, humAvg, tDiff, hDiff,
                    probability * 100.0f, isRain ? "RAIN" : "SUNNY");
            }
        } else {
            Serial.println("[TinyML] Invoke failed.");
        }

        // 5) Update T/H cu = trung binh batch vua xu ly
        prevBatchTempAvg = tempAvg;
        prevBatchHumAvg = humAvg;
        hasPrevBatchAvg = true;

        // 6) Reset batch de lap lai chu ky tiep theo
        sumTemp = 0.0f;
        sumHum = 0.0f;
        batchCount = 0;
    }
}