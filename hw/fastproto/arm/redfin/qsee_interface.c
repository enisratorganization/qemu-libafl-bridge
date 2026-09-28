/*
 * Redfin: SBL -> QSEE boot image table ("boot_sbl_qsee_interface").
 * Hooked at QSEE start to drop the hypervisor (QHEE) from the boot chain.
 */

#include "redfin.h"

#define QSEE_INTERFACE_ADDR 0x148FA220

typedef uint32_t uint32;
typedef uint64_t uint64;
struct _secboot_pbl_profile_marker
{
  uint32 secboot_pbl_authenticate_init_entry; ///< Start of secboot_authenticate entry
  uint32 secboot_pbl_parse_cert_buffer; ///< Start of parse_cert_buffer
  uint32 secboot_pbl_hash_certificates; ///< Start of hash certificates
  uint32 secboot_pbl_verify_cert_chain; ///< Start of verify cert chain
  uint32 secboot_pbl_verify_image_signature; ///< start of verify image signature
  uint32 secboot_pbl_authenticate_close; ///< secboot_authenticate is complete
} QEMU_PACKED;

typedef struct _secboot_pbl_profile_marker secboot_pbl_profile_marker_type;
typedef uint8_t uint8;
struct secboot_code_hash_info_type
{
  uint64 code_address;                  ///< Address (pointer value) of the code that was hashed
  uint32 code_length;                   ///< the code length
  uint32 image_hash_length;             ///< hash length - e.g 20 for SHA1, 32 for SHA256
  uint8 image_hash[32];                 ///< hash of HEADER + CODE
} QEMU_PACKED;

/* 52 */
typedef struct secboot_code_hash_info_type secboot_image_hash_info_type;

/* 53 */
struct secboot_verified_info_type
{
  uint32 version_id;                    ///< The version id (define the secboot lib version)
   uint64 sw_id; ///< The software id (upper 32 bits:version, lower 32 bits:type)
                                                                    ///< the image was signed with
  uint64 msm_hw_id; ///< The constructed MSM HW ID value used to authenticate
                                                                        ///< the image
  uint32 enable_debug;                  ///< Value of the debug settings from the attestation cert, i.e.,
                                        ///< SECBOOT_DEBUG_NOP, SECBOOT_DEBUG_DISABLE, SECBOOT_DEBUG_ENABLE
  secboot_image_hash_info_type image_hash_info; ///< Hash of the header + code
  uint32 enable_crash_dump;             ///< Value of the crash dump settings from the attestation cert i.e
                                        ///< SECBOOT_CRASH_DUMP_DISABLE, SECBOOT_CRASH_DUMP_ENABLE
  uint32 enable_root_key_control;       ///< Value to indicate if the configu fuse region is writeable
                                        ///< or not. i.e SECBOOT_ROOT_KEY_CONTROL_ENABLE,
                                        ///< SECBOOT_ROOT_KEY_CONTROL_DISABLE
  secboot_pbl_profile_marker_type secboot_timestamps; ///< Performance timestamps
} QEMU_PACKED;
typedef struct secboot_verified_info_type secboot_verified_info_type;

/* 54 */
struct boot_images_entry
{
  uint32 image_id;
  uint32 e_ident;
  uint64 entry_point;
  secboot_verified_info_type image_verified_info;
  uint32 reserved_1;
  uint32 reserved_2;
  uint32 reserved_3;
  uint32 reserved_4;
  char pad[136];
} QEMU_PACKED;
typedef struct boot_images_entry boot_images_entry;

/* 55 */
struct boot_sbl_qsee_interface
{
  uint32 magic_1;
  uint32 magic_2;
  uint32 version;
  uint32 number_images;
  uint32 reset_required;
  uint32_t field_14;
  boot_images_entry boot_image_entry[13];
  uint64 ddr_enter_self_refresh;
  uint64 ddr_exit_self_refresh;
  uint32 appsbl_entry_index;
  uint32 reserved_2;
} QEMU_PACKED;

typedef struct boot_sbl_qsee_interface boot_sbl_qsee_interface;

// Function to print images
static void print_images(boot_sbl_qsee_interface *qi) {
    FP_LOG("########## boot_sbl_qsee_interface START\n");
    for (int i = 0; i < MIN(qi->number_images, 13u); i++) {
        FP_LOG("Image id: %u entry: 0x%" PRIx64 "\n", qi->boot_image_entry[i].image_id, qi->boot_image_entry[i].entry_point);
    }
    FP_LOG("########## boot_sbl_qsee_interface END\n");
}

// Function to remove HYP from qsee interface
static void remove_hyp(CPUState *cs, boot_sbl_qsee_interface *qi) {
    if (qi->boot_image_entry[5].image_id != 21) {
        FP_LOG("qsee interface: Boot image #5 is not HYP!\n");
        return;
    }
    qi->boot_image_entry[5] = qi->boot_image_entry[6];
    qi->number_images = 6;
    qi->appsbl_entry_index = 5;

    fp_write(cs, QSEE_INTERFACE_ADDR, qi, sizeof(*qi));
    FP_LOG("Removed HYP from sbl_qsee_interface\n");
}

/* Hook at QSEE entry (see instr_tz.c) */
bool hook_qsee_start(CPUState *cs, vaddr pc, void *opaque)
{
    boot_sbl_qsee_interface qi;
    fp_read(cs, QSEE_INTERFACE_ADDR, &qi, sizeof(qi));
    print_images(&qi);

    // Remove HYP from qsee interface
    remove_hyp(cs, &qi);
    return false;
}
