// Copyright 2015-2017 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_vfs.h"
#include "esp_vfs_fat.h"
#include "diskio.h"

#include "wear_levelling.h"
#include "diskio_spiflash.h"

static const char *TAG = "vfs_fat_spiflash";
esp_err_t esp_vfs_fat_spiflash_mount(const char* base_path,
    const char* partition_label,
    const esp_vfs_fat_mount_config_t* mount_config,
    wl_handle_t* wl_handle)
{
    esp_err_t result = ESP_OK;
    const size_t workbuf_size = 4096;
    void *workbuf = NULL;

    esp_partition_t *data_partition = (esp_partition_t *)esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, partition_label);
    if (data_partition == NULL) {
        ESP_LOGE(TAG, "Failed to find FATFS partition (type='data', subtype='fat', partition_label='%s'). Check the partition table.", partition_label);
        return ESP_ERR_NOT_FOUND;
    }

    result = wl_mount(data_partition, wl_handle);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "failed to mount wear levelling layer. result = %i", result);
        return result;
    }
    // connect driver to FATFS
    BYTE pdrv = 0xFF;
    if (ff_diskio_get_drive(&pdrv) != ESP_OK || pdrv == 0xFF) {
        ESP_LOGD(TAG, "the maximum count of volumes is already mounted");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGD(TAG, "pdrv=%i\n", pdrv);

    char drv[3] = {(char)('0' + pdrv), ':', 0};

    result = ff_diskio_register_wl_partition(pdrv, *wl_handle);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "ff_diskio_register_wl_partition failed pdrv=%i, error - 0x(%x)", pdrv, result);
        goto fail;
    }
    FATFS *fs;
    result = esp_vfs_fat_register(base_path, drv, mount_config->max_files, &fs);
    if (result == ESP_ERR_INVALID_STATE) {
        // it's okay, already registered with VFS
    } else if (result != ESP_OK) {
        ESP_LOGD(TAG, "esp_vfs_fat_register failed 0x(%x)", result);
        goto fail;
    }

    // Try to mount partition
    FRESULT fresult = f_mount(fs, drv, 1);
    if (fresult != FR_OK) {
        ESP_LOGW(TAG, "f_mount failed (%d)", fresult);
        if (!(fresult == FR_NO_FILESYSTEM && mount_config->format_if_mount_failed)) {
            result = ESP_FAIL;
            goto fail;
        }
        workbuf = malloc(workbuf_size);
        ESP_LOGI(TAG, "Formatting FATFS partition");
        fresult = f_mkfs("", FM_ANY | FM_SFD, workbuf_size, workbuf, workbuf_size);
        if (fresult != FR_OK) {
            result = ESP_FAIL;
            ESP_LOGE(TAG, "f_mkfs failed (%d)", fresult);
            goto fail;
        }
        free(workbuf);
        ESP_LOGI(TAG, "Mounting again");
        fresult = f_mount(fs, drv, 0);
        if (fresult != FR_OK) {
            result = ESP_FAIL;
            ESP_LOGE(TAG, "f_mount failed after formatting (%d)", fresult);
            goto fail;
        }
    }
    return ESP_OK;

fail:
    free(workbuf);
    esp_vfs_fat_unregister_path(base_path);
    ff_diskio_unregister(pdrv);
    return result;
}

esp_err_t esp_vfs_fat_spiflash_unmount(const char *base_path, wl_handle_t wl_handle)
{
    BYTE s_pdrv = ff_diskio_get_pdrv_wl(wl_handle);
    char drv[3] = {(char)('0' + s_pdrv), ':', 0};

    f_mount(0, drv, 0);
    ff_diskio_unregister(s_pdrv);
    // release partition driver
    esp_err_t err_drv = wl_unmount(wl_handle);
    esp_err_t err = esp_vfs_fat_unregister_path(base_path);
    if (err == ESP_OK) err = err_drv;
    return err;
}
