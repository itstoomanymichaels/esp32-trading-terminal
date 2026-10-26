#include <Arduino.h>

constexpr UBaseType_t QUEUE_LEN = 5;
static QueueHandle_t g_queue;

void producerTask(void *pvParameters) {
  int counter = 0;
  for (;;) {
    if (xQueueSend(g_queue, &counter, 0) != pdTRUE)
      Serial.println("Queue full, dropped item");
    counter++;
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void consumerTask(void *pvParameters) {
  int received;
  for (;;) {
    if (xQueueReceive(g_queue, &received, portMAX_DELAY) == pdTRUE)
      Serial.printf("Got %d\n", received);
  }
}

void setup() {
  Serial.begin(115200);
  g_queue = xQueueCreate(QUEUE_LEN, sizeof(int));
  if (g_queue == NULL) {
    Serial.println("Queue creation failed");
    while (true)
      vTaskDelay(pdMS_TO_TICKS(1000));
  }

  xTaskCreate(producerTask, "producer", 4096, NULL, 1, NULL);
  xTaskCreate(consumerTask, "consumer", 4096, NULL, 1, NULL);
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}