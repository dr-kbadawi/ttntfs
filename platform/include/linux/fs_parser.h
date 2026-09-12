/* SPDX-License-Identifier: GPL-2.0 */
/* Mount option parser. The port receives options through ntfscore.h, but
 * super.c's table-driven parser is kept so its semantics survive. */
#ifndef _LINUX_FS_PARSER_H
#define _LINUX_FS_PARSER_H
#include <linux/types.h>
#include <linux/fs_context.h>
struct constant_table { const char *name; int value; };
struct fs_parameter_spec;
struct p_log;
struct fs_parse_result { bool negated; union { bool boolean; int int_32; unsigned int uint_32; u64 uint_64; kuid_t uid; kgid_t gid; }; };
typedef int fs_param_type(struct p_log *, const struct fs_parameter_spec *, struct fs_parameter *, struct fs_parse_result *);
struct fs_parameter_spec { const char *name; fs_param_type *type; u8 opt; unsigned short flags; const void *data; };
#define fs_param_neg_with_no 0x0002
#define fs_param_can_be_empty 0x0004
enum fs_param_kind { FSP_FLAG = 1, FSP_BOOL, FSP_U32, FSP_U32OCT, FSP_U32HEX, FSP_S32, FSP_U64, FSP_ENUM, FSP_STRING, FSP_BLOB, FSP_BDEV, FSP_PATH, FSP_FD, FSP_UID, FSP_GID };
/* Encode the kind in the type pointer slot via distinct dummy functions. */
int fs_param_is_flag(struct p_log *, const struct fs_parameter_spec *, struct fs_parameter *, struct fs_parse_result *);
int fs_param_is_bool(struct p_log *, const struct fs_parameter_spec *, struct fs_parameter *, struct fs_parse_result *);
int fs_param_is_u32(struct p_log *, const struct fs_parameter_spec *, struct fs_parameter *, struct fs_parse_result *);
int fs_param_is_u32oct(struct p_log *, const struct fs_parameter_spec *, struct fs_parameter *, struct fs_parse_result *);
int fs_param_is_u32hex(struct p_log *, const struct fs_parameter_spec *, struct fs_parameter *, struct fs_parse_result *);
int fs_param_is_s32(struct p_log *, const struct fs_parameter_spec *, struct fs_parameter *, struct fs_parse_result *);
int fs_param_is_u64(struct p_log *, const struct fs_parameter_spec *, struct fs_parameter *, struct fs_parse_result *);
int fs_param_is_enum(struct p_log *, const struct fs_parameter_spec *, struct fs_parameter *, struct fs_parse_result *);
int fs_param_is_string(struct p_log *, const struct fs_parameter_spec *, struct fs_parameter *, struct fs_parse_result *);
int fs_param_is_uid(struct p_log *, const struct fs_parameter_spec *, struct fs_parameter *, struct fs_parse_result *);
int fs_param_is_gid(struct p_log *, const struct fs_parameter_spec *, struct fs_parameter *, struct fs_parse_result *);
#define __fsparam(TYPE, NAME, OPT, FLAGS, DATA) { .name = NAME, .type = TYPE, .opt = OPT, .flags = FLAGS, .data = DATA }
#define fsparam_flag(NAME, OPT) __fsparam(NULL, NAME, OPT, 0, NULL)
#define fsparam_flag_no(NAME, OPT) __fsparam(NULL, NAME, OPT, fs_param_neg_with_no, NULL)
#define fsparam_bool(NAME, OPT) __fsparam(fs_param_is_bool, NAME, OPT, 0, NULL)
#define fsparam_u32(NAME, OPT) __fsparam(fs_param_is_u32, NAME, OPT, 0, NULL)
#define fsparam_u32oct(NAME, OPT) __fsparam(fs_param_is_u32oct, NAME, OPT, 0, (void *)8)
#define fsparam_u32hex(NAME, OPT) __fsparam(fs_param_is_u32hex, NAME, OPT, 0, (void *)16)
#define fsparam_s32(NAME, OPT) __fsparam(fs_param_is_s32, NAME, OPT, 0, NULL)
#define fsparam_u64(NAME, OPT) __fsparam(fs_param_is_u64, NAME, OPT, 0, NULL)
#define fsparam_enum(NAME, OPT, array) __fsparam(fs_param_is_enum, NAME, OPT, 0, array)
#define fsparam_string(NAME, OPT) __fsparam(fs_param_is_string, NAME, OPT, 0, NULL)
#define fsparam_uid(NAME, OPT) __fsparam(fs_param_is_uid, NAME, OPT, 0, NULL)
#define fsparam_gid(NAME, OPT) __fsparam(fs_param_is_gid, NAME, OPT, 0, NULL)
int fs_parse(struct fs_context *fc, const struct fs_parameter_spec *desc, struct fs_parameter *param, struct fs_parse_result *result);
int fs_lookup_param(struct fs_context *fc, struct fs_parameter *param, bool want_bdev, unsigned int flags, void *path);
#endif
