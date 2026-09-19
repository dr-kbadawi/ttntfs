/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2026 TechTag GmbH */
/*
 * Mount-option parsing (linux/fs_parser.h) and the mount entry point
 * get_tree_bdev() (linux/fs_context.h).
 *
 * fs_parse() matches a parameter against the spec table the way the kernel
 * does: "noX" negates a fs_param_neg_with_no flag, a flag takes no value,
 * everything else requires one and is converted by the spec's type function.
 * It returns the spec's opt number, -ENOPARAM for an unknown key, or a
 * negative errno for a bad value.
 */
#include <stdlib.h>
#include <string.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/fs_parser.h>
#include <ntfsport/bdev.h>

/* ------------------------------------------------------------------ */
/* Type functions                                                      */

#define param_string(p) ((p)->type == fs_value_is_string && (p)->string ? (p)->string : NULL)

int fs_param_is_flag(struct p_log *log, const struct fs_parameter_spec *spec,
		     struct fs_parameter *param, struct fs_parse_result *result)
{
	(void)log; (void)spec; (void)param;
	result->boolean = !result->negated;
	return 0;
}

int fs_param_is_bool(struct p_log *log, const struct fs_parameter_spec *spec,
		     struct fs_parameter *param, struct fs_parse_result *result)
{
	const char *s = param_string(param);
	(void)log; (void)spec;
	if (!s) {
		result->boolean = true;
		return 0;
	}
	if (!strcmp(s, "1") || !strcmp(s, "yes") || !strcmp(s, "true") || !strcmp(s, "on"))
		result->boolean = true;
	else if (!strcmp(s, "0") || !strcmp(s, "no") || !strcmp(s, "false") || !strcmp(s, "off"))
		result->boolean = false;
	else
		return -EINVAL;
	return 0;
}

static int parse_u32_base(struct fs_parameter *param, struct fs_parse_result *result, unsigned int base)
{
	const char *s = param_string(param);
	if (!s)
		return -EINVAL;
	return kstrtou32(s, base, &result->uint_32);
}

int fs_param_is_u32(struct p_log *log, const struct fs_parameter_spec *spec,
		    struct fs_parameter *param, struct fs_parse_result *result)
{
	(void)log;
	return parse_u32_base(param, result, (unsigned int)(unsigned long)spec->data);
}

int fs_param_is_u32oct(struct p_log *log, const struct fs_parameter_spec *spec,
		       struct fs_parameter *param, struct fs_parse_result *result)
{
	(void)log; (void)spec;
	return parse_u32_base(param, result, 8);
}

int fs_param_is_u32hex(struct p_log *log, const struct fs_parameter_spec *spec,
		       struct fs_parameter *param, struct fs_parse_result *result)
{
	(void)log; (void)spec;
	return parse_u32_base(param, result, 16);
}

int fs_param_is_s32(struct p_log *log, const struct fs_parameter_spec *spec,
		    struct fs_parameter *param, struct fs_parse_result *result)
{
	const char *s = param_string(param);
	(void)log; (void)spec;
	if (!s)
		return -EINVAL;
	return kstrtos32(s, 0, &result->int_32);
}

int fs_param_is_u64(struct p_log *log, const struct fs_parameter_spec *spec,
		    struct fs_parameter *param, struct fs_parse_result *result)
{
	const char *s = param_string(param);
	(void)log; (void)spec;
	if (!s)
		return -EINVAL;
	return kstrtou64(s, 0, &result->uint_64);
}

int fs_param_is_enum(struct p_log *log, const struct fs_parameter_spec *spec,
		     struct fs_parameter *param, struct fs_parse_result *result)
{
	const struct constant_table *t = spec->data;
	const char *s = param_string(param);
	(void)log;
	if (!s || !t)
		return -EINVAL;
	for (; t->name; t++) {
		if (!strcmp(t->name, s)) {
			result->uint_32 = (unsigned int)t->value;
			return 0;
		}
	}
	return -EINVAL;
}

int fs_param_is_string(struct p_log *log, const struct fs_parameter_spec *spec,
		       struct fs_parameter *param, struct fs_parse_result *result)
{
	const char *s = param_string(param);
	(void)log; (void)result;
	if (!s || (!*s && !(spec->flags & fs_param_can_be_empty)))
		return -EINVAL;
	return 0;
}

int fs_param_is_uid(struct p_log *log, const struct fs_parameter_spec *spec,
		    struct fs_parameter *param, struct fs_parse_result *result)
{
	u32 v;
	int err = fs_param_is_u32(log, spec, param, result);
	if (err)
		return err;
	v = result->uint_32;
	result->uid = KUIDT_INIT((uid_t)v);
	return 0;
}

int fs_param_is_gid(struct p_log *log, const struct fs_parameter_spec *spec,
		    struct fs_parameter *param, struct fs_parse_result *result)
{
	u32 v;
	int err = fs_param_is_u32(log, spec, param, result);
	if (err)
		return err;
	v = result->uint_32;
	result->gid = KGIDT_INIT((gid_t)v);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Parser                                                              */

int fs_parse(struct fs_context *fc, const struct fs_parameter_spec *desc,
	     struct fs_parameter *param, struct fs_parse_result *result)
{
	const struct fs_parameter_spec *p, *spec = NULL;
	bool negated = false;
	int err;

	(void)fc;
	if (!param || !param->key)
		return -EINVAL;
	memset(result, 0, sizeof(*result));
	for (p = desc; p->name; p++) {
		if (!strcmp(p->name, param->key)) {
			spec = p;
			break;
		}
	}
	if (!spec && param->key[0] == 'n' && param->key[1] == 'o') {
		for (p = desc; p->name; p++) {
			if ((p->flags & fs_param_neg_with_no) && !strcmp(p->name, param->key + 2)) {
				spec = p;
				negated = true;
				break;
			}
		}
	}
	if (!spec)
		return -ENOPARAM;
	result->negated = negated;

	if (!spec->type) {
		/* A flag: no value allowed. */
		if (param->type != fs_value_is_flag &&
		    !(param->type == fs_value_is_string && param->string && !*param->string))
			return -EINVAL;
		result->boolean = !negated;
		return spec->opt;
	}
	if (negated)
		return -EINVAL;
	if (param->type == fs_value_is_flag && !(spec->flags & fs_param_can_be_empty)) {
		/* Booleans accept a bare key ("acl" == "acl=1"). */
		if (spec->type != fs_param_is_bool)
			return -EINVAL;
	}
	err = spec->type(NULL, spec, param, result);
	if (err)
		return err;
	return spec->opt;
}

int fs_lookup_param(struct fs_context *fc, struct fs_parameter *param, bool want_bdev,
		    unsigned int flags, void *path)
{
	(void)fc; (void)param; (void)want_bdev; (void)flags; (void)path;
	return -ENOENT;
}

/* ------------------------------------------------------------------ */
/* Mount                                                               */

int get_tree_bdev(struct fs_context *fc, int (*fill_super)(struct super_block *sb, struct fs_context *fc))
{
	struct super_block *sb;
	int err;

	if (!fc->bdev)
		return -ENODEV;
	sb = sb_alloc();
	if (!sb)
		return -ENOMEM;
	sb->s_bdev = fc->bdev;
	sb->s_flags |= fc->sb_flags & ~(unsigned long)SB_ACTIVE;
	if (fc->bdev->read_only)
		sb->s_flags |= SB_RDONLY;
	strlcpy(sb->s_id, fc->bdev->name, sizeof(sb->s_id));
	sb->s_fs_info = fc->s_fs_info;
	fc->s_fs_info = NULL;
	sb_min_blocksize(sb, 512);

	err = fill_super(sb, fc);
	if (err) {
		/* fill_super released s_fs_info on its error path (super.c
		 * does); anything it left behind is the caller's. */
		sb_free(sb);
		return err;
	}
	sb->s_flags |= SB_ACTIVE;
	fc->root = sb->s_root;
	return 0;
}
