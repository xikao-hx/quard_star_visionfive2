#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "bh_ota_package.h"

int main(int argc, char **argv)
{
    bh_ota_manifest_t manifest;
    char manifest_path[512];
    char signature_path[512];
    int32_t compare_result = 0;
    int written;

    if (argc != 4) {
        fprintf(stderr, "Usage: %s <package_dir> <device_version_file> <public_key>\n", argv[0]);
        return 1;
    }

    written = snprintf(manifest_path, sizeof(manifest_path), "%s/%s", argv[1], BH_OTA_MANIFEST_FILE_NAME);
    assert(written > 0 && (size_t)written < sizeof(manifest_path));
    written = snprintf(signature_path, sizeof(signature_path), "%s/%s", argv[1], BH_OTA_SIGNATURE_FILE_NAME);
    assert(written > 0 && (size_t)written < sizeof(signature_path));

    assert(bh_ota_manifest_parse_file(manifest_path, &manifest) == BH_OTA_OK);
    assert(bh_ota_manifest_check_version_file(&manifest, argv[2], &compare_result) == BH_OTA_OK);
    assert(compare_result >= 0);
    assert(bh_ota_manifest_verify_payloads(&manifest, argv[1]) == BH_OTA_OK);
    assert(bh_ota_manifest_verify_signature(&manifest, argv[1], signature_path, argv[3]) == BH_OTA_OK);

    puts("test_bh_ota_package: all checks passed");
    return 0;
}
