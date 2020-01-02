/*
 * Copyright 2010-2017, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * This file contains C code routines that used to generate VDBE code
 * that implements the ALTER TABLE command.
 */
#include "sqlInt.h"
#include "box/box.h"
#include "box/schema.h"
#include "tarantoolInt.h"

void
sql_alter_table_rename(struct Parse *parse)
{
	struct rename_entity_def *rename_def = &parse->rename_entity_def;
	struct SrcList *src_tab = rename_def->base.entity_name;
	assert(rename_def->base.entity_type == ENTITY_TYPE_TABLE);
	assert(rename_def->base.alter_action == ALTER_ACTION_RENAME);
	assert(src_tab->nSrc == 1);
	struct sql *db = parse->db;
	char *new_name = sql_name_from_token(db, &rename_def->new_name);
	if (new_name == NULL)
		goto tnt_error;
	/* Check that new name isn't occupied by another table. */
	if (space_by_name(new_name) != NULL) {
		diag_set(ClientError, ER_SPACE_EXISTS, new_name);
		goto tnt_error;
	}
	const char *tbl_name = src_tab->a[0].zName;
	struct space *space = space_by_name(tbl_name);
	if (space == NULL) {
		diag_set(ClientError, ER_NO_SUCH_SPACE, tbl_name);
		goto tnt_error;
	}
	if (space->def->opts.is_view) {
		diag_set(ClientError, ER_ALTER_SPACE, tbl_name,
			 "view may not be altered");
		goto tnt_error;
	}
	sql_set_multi_write(parse, false);
	/* Drop and reload the internal table schema. */
	struct Vdbe *v = sqlGetVdbe(parse);
	sqlVdbeAddOp4(v, OP_RenameTable, space->def->id, 0, 0, new_name,
			  P4_DYNAMIC);
exit_rename_table:
	sqlSrcListDelete(db, src_tab);
	return;
tnt_error:
	sqlDbFree(db, new_name);
	parse->is_aborted = true;
	goto exit_rename_table;
}

void
sql_alter_ck_constraint_enable(struct Parse *parse)
{
	struct enable_entity_def *enable_def = &parse->enable_entity_def;
	struct SrcList *src_tab = enable_def->base.entity_name;
	assert(enable_def->base.entity_type == ENTITY_TYPE_CK);
	assert(enable_def->base.alter_action == ALTER_ACTION_ENABLE);
	assert(src_tab->nSrc == 1);
	struct sql *db = parse->db;

	char *constraint_name = NULL;
	const char *tbl_name = src_tab->a[0].zName;
	struct space *space = space_by_name(tbl_name);
	if (space == NULL) {
		diag_set(ClientError, ER_NO_SUCH_SPACE, tbl_name);
		parse->is_aborted = true;
		goto exit_alter_ck_constraint;
	}

	constraint_name = sql_name_from_token(db, &enable_def->name);
	if (constraint_name == NULL) {
		parse->is_aborted = true;
		goto exit_alter_ck_constraint;
	}

	struct Vdbe *v = sqlGetVdbe(parse);
	if (v == NULL)
		goto exit_alter_ck_constraint;

	struct space *ck_space = space_by_id(BOX_CK_CONSTRAINT_ID);
	assert(ck_space != NULL);
	int cursor = parse->nTab++;
	vdbe_emit_open_cursor(parse, cursor, 0, ck_space);
	sqlVdbeChangeP5(v, OPFLAG_SYSTEMSP);

	int key_reg = sqlGetTempRange(parse, 2);
	sqlVdbeAddOp2(v, OP_Integer, space->def->id, key_reg);
	sqlVdbeAddOp4(v, OP_String8, 0, key_reg + 1, 0,
		      sqlDbStrDup(db, constraint_name), P4_DYNAMIC);
	int addr = sqlVdbeAddOp4Int(v, OP_Found, cursor, 0, key_reg, 2);
	sqlVdbeAddOp4(v, OP_SetDiag, ER_NO_SUCH_CONSTRAINT, 0, 0,
		      sqlMPrintf(db, tnt_errcode_desc(ER_NO_SUCH_CONSTRAINT),
				 constraint_name), P4_DYNAMIC);
	sqlVdbeAddOp2(v, OP_Halt, -1, ON_CONFLICT_ACTION_ABORT);
	sqlVdbeJumpHere(v, addr);

	const int field_count = 6;
	int tuple_reg = sqlGetTempRange(parse, field_count + 1);
	for (int i = 0; i < field_count - 1; ++i)
		sqlVdbeAddOp3(v, OP_Column, cursor, i, tuple_reg + i);
	sqlVdbeAddOp1(v, OP_Close, cursor);
	sqlVdbeAddOp2(v, OP_Bool, enable_def->is_enabled,
		      tuple_reg + field_count - 1);
	sqlVdbeAddOp3(v, OP_MakeRecord, tuple_reg, field_count,
		      tuple_reg + field_count);
	sqlVdbeAddOp4(v, OP_IdxReplace, tuple_reg + field_count, 0, 0,
		      (char *)ck_space, P4_SPACEPTR);
exit_alter_ck_constraint:
	sqlDbFree(db, constraint_name);
	sqlSrcListDelete(db, src_tab);
}

/* This function is used to implement the ALTER TABLE command.
 * The table name in the CREATE TRIGGER statement is replaced with the third
 * argument and the result returned. This is analagous to rename_table()
 * above, except for CREATE TRIGGER, not CREATE INDEX and CREATE TABLE.
 */
char*
rename_trigger(sql *db, char const *sql_stmt, char const *table_name,
	       bool *is_quoted)
{
	assert(sql_stmt);
	assert(table_name);
	assert(is_quoted);

	int token;
	Token tname;
	int dist = 3;
	char const *csr = (char const*)sql_stmt;
	int len = 0;
	char *new_sql_stmt;
	bool unused;

	/* The principle used to locate the table name in the CREATE TRIGGER
	 * statement is that the table name is the first token that is immediately
	 * preceded by either TK_ON or TK_DOT and immediately followed by one
	 * of TK_WHEN, TK_BEGIN or TK_FOR.
	 */
	do {
		if (!*csr) {
			/* Ran out of input before finding the table name. */
			return NULL;
		}
		/* Store the token that csr points to in tname. */
		tname.z = (char *) csr;
		tname.n = len;
		/* Advance zCsr to the next token. Store that token type in 'token',
		 * and its length in 'len' (to be used next iteration of this loop).
		 */
		do {
			csr += len;
			len = sql_token(csr, &token, &unused);
		} while (token == TK_SPACE);
		assert(len > 0);
		/* Variable 'dist' stores the number of tokens read since the most
		 * recent TK_ON. This means that when a WHEN, FOR or BEGIN
		 * token is read and 'dist' equals 2, the condition stated above
		 * to be met.
		 *
		 * Note that ON cannot be a table or column name, so
		 * there is no need to worry about syntax like
		 * "CREATE TRIGGER ... ON ON BEGIN ..." etc.
		 */
		dist++;
		if (token == TK_ON) {
			dist = 0;
		}
	} while (dist != 2 ||
		 (token != TK_WHEN && token != TK_FOR && token != TK_BEGIN));

	if (*tname.z  == '"')
		*is_quoted = true;
	/* Variable tname now contains the token that is the old table-name
	 * in the CREATE TRIGGER statement.
	 */
	new_sql_stmt = sqlMPrintf(db, "%.*s\"%w\"%s",
				      (int)((tname.z) - sql_stmt), sql_stmt,
				      table_name, tname.z + tname.n);
	return new_sql_stmt;
}

void
sql_alter_add_column_start(struct Parse *parse)
{
	struct create_column_def *create_column_def = &parse->create_column_def;
	struct alter_entity_def *alter_entity_def =
		&create_column_def->base.base;
	assert(alter_entity_def->entity_type == ENTITY_TYPE_COLUMN);
	assert(alter_entity_def->alter_action == ALTER_ACTION_CREATE);

	const char *space_name =
		alter_entity_def->entity_name->a[0].zName;
	struct space *space = space_by_name(space_name);
	struct sql *db = parse->db;
	if (space == NULL) {
		diag_set(ClientError, ER_NO_SUCH_SPACE, space_name);
		goto tnt_error;
	}

	struct space_def *def = space->def;
	create_column_def->space_def = def;
	if (sql_space_def_check_format(def) != 0)
		goto tnt_error;
	uint32_t new_field_count = def->field_count + 1;
	create_column_def->new_field_count = new_field_count;

#if SQL_MAX_COLUMN
	if ((int)new_field_count > db->aLimit[SQL_LIMIT_COLUMN]) {
		diag_set(ClientError, ER_SQL_COLUMN_COUNT_MAX, def->name,
			 new_field_count, db->aLimit[SQL_LIMIT_COLUMN]);
		goto tnt_error;
	}
#endif
	/*
	 * Create new fields array and add the new column's
	 * field_def to its end. During copying don't make
	 * duplicates of the non-scalar fields (name,
	 * default_value, default_value_expr), because they will
	 * be just copied and encoded to the msgpack array on sql
	 * allocator in sql_alter_add_column_end().
	 */
	uint32_t size = sizeof(struct field_def) * new_field_count;
	struct region *region = &parse->region;
	struct field_def *new_fields = region_alloc(region, size);
	if (new_fields == NULL) {
		diag_set(OutOfMemory, size, "region_alloc", "new_fields");
		goto tnt_error;
	}
	memcpy(new_fields, def->fields,
	       sizeof(struct field_def) * def->field_count);
	create_column_def->new_fields = new_fields;
	struct field_def *new_column_def = &new_fields[def->field_count];

	memcpy(new_column_def, &field_def_default, sizeof(struct field_def));
	struct Token *name = &create_column_def->base.name;
	new_column_def->name =
		sql_normalized_name_region_new(region, name->z, name->n);
	if (new_column_def->name == NULL)
		goto tnt_error;
	if (def->opts.is_view) {
		diag_set(ClientError, ER_SQL_CANT_ADD_COLUMN_TO_VIEW,
			 new_column_def->name, def->name);
		goto tnt_error;
	}

	new_column_def->type = create_column_def->type_def->type;

exit_alter_add_column_start:
	sqlSrcListDelete(db, alter_entity_def->entity_name);
	return;
tnt_error:
	parse->is_aborted = true;
	goto exit_alter_add_column_start;
}

void
sql_alter_add_column_end(struct Parse *parse)
{
	struct create_column_def *create_column_def = &parse->create_column_def;
	uint32_t new_field_count = create_column_def->new_field_count;
	uint32_t table_stmt_sz = 0;
	char *table_stmt =
		sql_encode_table_format(&parse->region,
					create_column_def->new_fields,
					new_field_count, &table_stmt_sz);
	if (table_stmt == NULL) {
		parse->is_aborted = true;
		return;
	}
	char *raw = sqlDbMallocRaw(parse->db, table_stmt_sz);
	if (raw == NULL){
		parse->is_aborted = true;
		return;
	}
	memcpy(raw, table_stmt, table_stmt_sz);

	struct space *system_space = space_by_id(BOX_SPACE_ID);
	assert(system_space != NULL);
	int cursor = parse->nTab++;
	struct Vdbe *v = sqlGetVdbe(parse);
	assert(v != NULL);
	vdbe_emit_open_cursor(parse, cursor, 0, system_space);
	sqlVdbeChangeP5(v, OPFLAG_SYSTEMSP);

	int key_reg = sqlGetTempReg(parse);
	sqlVdbeAddOp2(v, OP_Integer, create_column_def->space_def->id, key_reg);
	int addr = sqlVdbeAddOp4Int(v, OP_Found, cursor, 0, key_reg, 1);
	sqlVdbeAddOp2(v, OP_Halt, -1, ON_CONFLICT_ACTION_ABORT);
	sqlVdbeJumpHere(v, addr);

	const int system_space_field_count = 7;
	int tuple_reg = sqlGetTempRange(parse, system_space_field_count + 1);
	for (int i = 0; i < system_space_field_count - 1; ++i)
		sqlVdbeAddOp3(v, OP_Column, cursor, i, tuple_reg + i);
	sqlVdbeAddOp1(v, OP_Close, cursor);

	sqlVdbeAddOp2(v, OP_Integer, new_field_count, tuple_reg + 4);
	sqlVdbeAddOp4(v, OP_Blob, table_stmt_sz, tuple_reg + 6,
		      SQL_SUBTYPE_MSGPACK, raw, P4_DYNAMIC);
	sqlVdbeAddOp3(v, OP_MakeRecord, tuple_reg, system_space_field_count,
		      tuple_reg + system_space_field_count);
	sqlVdbeAddOp4(v, OP_IdxReplace, tuple_reg + system_space_field_count, 0,
		      0, (char *)system_space, P4_SPACEPTR);
}
