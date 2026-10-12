#include "storage.h"
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_spiffs.h"

#define PRISMA_MAX_SOURCES 32
#define PRISMA_SOURCE_NAME_MAX 31

static const char *TAG = "PRISMA_STORAGE";
static char sources[PRISMA_MAX_SOURCES][PRISMA_SOURCE_NAME_MAX + 1];
static int source_count = 0;
static int source_index = 0;

static bool source_is_jpeg(const char *name){const char *dot = strrchr(name, '.');return dot && (!strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg"));}
static void sort_sources(void){for(int i=0;i<source_count-1;i++){for(int j=i+1;j<source_count;j++){if(strcasecmp(sources[i], sources[j]) > 0){char tmp[PRISMA_SOURCE_NAME_MAX + 1];strlcpy(tmp, sources[i], sizeof(tmp));strlcpy(sources[i], sources[j], sizeof(sources[i]));strlcpy(sources[j], tmp, sizeof(sources[j]));}}}}

void prisma_storage_init(void){
    source_count = 0;source_index = 0;
    esp_vfs_spiffs_conf_t conf = {.base_path = "/storage",.partition_label = "storage",.max_files = 32,.format_if_mount_failed = false};
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(err));return;}
    size_t total = 0, used = 0;
    if (esp_spiffs_info("storage", &total, &used) == ESP_OK) ESP_LOGI(TAG, "SPIFFS mounted: total=%u used=%u", (unsigned)total, (unsigned)used);
    DIR *dir = opendir("/storage");if (!dir) {ESP_LOGE(TAG, "Cannot open /storage");return;}
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && source_count < PRISMA_MAX_SOURCES) {
        size_t len = strlen(entry->d_name);if(len == 0 || len > PRISMA_SOURCE_NAME_MAX || !source_is_jpeg(entry->d_name)) continue;
        char path[48];int n = snprintf(path, sizeof(path), "/storage/%.*s", PRISMA_SOURCE_NAME_MAX, entry->d_name);if(n <= 0 || n >= (int)sizeof(path)) continue;
        struct stat st;if(stat(path, &st) != 0) continue;strlcpy(sources[source_count], entry->d_name, sizeof(sources[source_count]));source_count++;
    }
    closedir(dir);sort_sources();for(int i=0;i<source_count;i++) if(!strcasecmp(sources[i], "F1_MOON.JPG")){source_index = i;break;}ESP_LOGI(TAG, "SOURCE COUNT=%d", source_count);
}

int prisma_storage_count(void){return source_count;}
const char *prisma_storage_current(void){if(source_count <= 0) return "F1_MOON.JPG";return sources[source_index];}
const char *prisma_storage_next(void){if(source_count <= 0) return "F1_MOON.JPG";source_index = (source_index + 1) % source_count;ESP_LOGI(TAG, "SOURCE -> %s", sources[source_index]);return sources[source_index];}
