#include "postgres.h"

#include "catalog/namespace.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "parser/parser.h"      /* only needed for GUC variables */
#include "parser/parse_type.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "pltsql.h"
#include "storage/lock.h"
#include "utils/builtins.h"
#include "utils/elog.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "datatypes.h"

bool suppress_string_truncation_error = false;

bool pltsql_suppress_string_truncation_error(void);

bool is_tsql_any_char_datatype(Oid oid); /* sys.char / sys.nchar / sys.varchar / sys.nvarchar */
bool is_tsql_text_ntext_or_image_datatype(Oid oid);

/* 
 * Following the rule for locktag fields of advisory locks:
 *	field1: MyDatabaseId ... ensures locks are local to each database
 *	field2: high-order half of an int8 key
 *	field3: low-order half of an int8 key
 *	field4: 1 or 2 are used in advisory lock funcs that user may call, so we use 3
 *	We also add a magic number to the key to avoid collision
 */
const uint64 PLTSQL_LOCKTAG_OFFSET = 0xABCDEF;
#define SET_LOCKTAG_INT16(tag, key16) \
	SET_LOCKTAG_ADVISORY(tag, \
						 MyDatabaseId, \
						 (uint32) ((((int64) key16) + PLTSQL_LOCKTAG_OFFSET) >> 32), \
						 (uint32) (((int64) key16) + PLTSQL_LOCKTAG_OFFSET), \
						 3)

/*
 * Setup default typmod for sys types/domains when typmod isn't specified
 * (that is, typmod = -1).
 * We only care to do this in TSQL dialect, this means sys.varchar
 * defaults to sys.varchar(1) only in TSQL dialect.
 *
 * is_cast indicates if it's a CAST/CONVERT statement, if it's true the default
 * length of string and binary type will be set to 30.
 *
 * If typmod is TSQLMaxTypmod (-8000), it means MAX is used in the
 * length field of VARCHAR, NVARCHAR or VARBINARY. Set typmod to -1,
 * by default -1 the engine will treat it as unlimited length.
 *
 * Also, length should be restricted to 8000 for sys.varchar and sys.char datatypes.
 * And length should be restricted to 4000 for sys.varchar and sys.char datatypes
 */
void 
pltsql_check_or_set_default_typmod(TypeName * typeName, int32 *typmod, bool is_cast)
{
	Assert(sql_dialect == SQL_DIALECT_TSQL);

	/* Do nothing for  internally generated TypeName or %TYPE */
	if (typeName->names == NIL || typeName->pct_type)
	{
		return;
	}
	else
	{
		/* Normal reference to a type name */
		char *schemaname;
		char *typname;
		bool	is_sys_schema = false;

		/* deconstruct the name list */
		DeconstructQualifiedName(typeName->names, &schemaname, &typname);
		if (schemaname)
			is_sys_schema = strcmp("sys", schemaname) == 0;
		else
		{
			Oid schema_oid;
			Oid sys_oid = InvalidOid;

			/* Unqualified type name, search the search path */
			schema_oid = typenameGetSchemaOID(typname, true);
			if (!OidIsValid(sys_oid))
				sys_oid = get_namespace_oid("sys", true);
			is_sys_schema = sys_oid == schema_oid;
		}
		if (is_sys_schema)
		{
			int max_allowed_varchar_length = 8000;
			int max_allowed_nvarchar_length = 4000;
			/* sys types/domains without typmod specification, set the default accordingly */
			if (*typmod == -1)
			{
				if (strcmp(typname, "varchar") == 0 ||
					strcmp(typname, "nvarchar") == 0 ||
					strcmp(typname, "nchar") == 0 ||
					strcmp(typname, "varbinary") == 0 ||
					strcmp(typname, "binary") == 0 ||
					strcmp(typname, "bpchar") == 0)
				{
					/* Default length is 30 in cast and convert statement */
					if (is_cast)
						/* atttypmod is the declared length of the type plus VARHDRSZ. */
						*typmod = 30 + VARHDRSZ;
					else
						/* Default length is 1 in the general case */
						*typmod = 1 + VARHDRSZ;
				}
				else if (strcmp(typname, "smalldatetime") == 0)
					*typmod = 0;
				else if (strcmp(typname, "decimal") == 0)
					*typmod = 1179652;  /* decimal(18,0) */
			}
			/* for sys.varchar/nvarchar/varbinary(MAX), set typmod back to -1 */
			else if (*typmod == TSQLMaxTypmod)
			{
				if (strcmp(typname, "varchar") == 0 ||
					strcmp(typname, "nvarchar") == 0 ||
					strcmp(typname, "varbinary") == 0)
					*typmod = -1;
				else
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							errmsg("Incorrect syntax near the keyword '%s'.", typname)));
			}
			else if (*typmod > (max_allowed_varchar_length + VARHDRSZ) && (strcmp(typname, "varchar") == 0 || strcmp(typname, "bpchar") == 0))
			{
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("The size '%d' exceeds the maximum allowed (%d) for '%s' datatype.",
							*typmod - VARHDRSZ, max_allowed_varchar_length, typname)));
			}
			else if (*typmod > (max_allowed_nvarchar_length + VARHDRSZ) && (strcmp(typname, "nvarchar") == 0 || strcmp(typname, "nchar") == 0))
			{
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("The size '%d' exceeds the maximum allowed (%d) for '%s' datatype.",
							*typmod - VARHDRSZ, max_allowed_nvarchar_length, typname)));
			}
		}
	}
}
/*
 * Declare variable API
 *
 * Given a variable's info, build its InlineCodeBlockArgs and FunctionCallInfo
 * Note that you still need to manually fill in the first two argumetns of fcinfo.
 * fcinfo->args[0] is the query string. fcinfo->args[1] is the
 * InlineCodeBlockAgs built here.
 *
 * Sample code for calling this function:
 * 
 * InlineCodeBlock *codeblock = ...;
 * InlineCodeBlockArgs *args = ...;
 * LOCAL_FCINFO(fcinfo, FUNC_MAX_ARGS);
 * MemSet(fcinfo, ...);
 *
 * fcinfo->flinfo = ...;
 * fcinfo->args[0].value = PointerGetDatum(codeblock);
 * fcinfo->args[0].isnull = false;
 * fcinfo->nargs = 1;
 *
 * for (p in params)
 *	pltsql_declare_variable(..., &args, &fcinfo);
 *
 * fcinfo->args[1].value = PointerGetDatum(args);
 * fcinfo->args[1].isnull = false;
 * fcinfo->nargs++;
 */
void pltsql_declare_variable(Oid type, int32 typmod, char *name, char mode, Datum value, 
			     bool isnull, int index, InlineCodeBlockArgs **args, 
			     FunctionCallInfo *fcinfo)
{
	/*
	 * In case of sp_execute, we don't need the following info.  Hence, skip
	 * filling InlineCodeBlockArgs if it's not provided.
	 */
	if(args)
	{
		(*args)->argtypes[index] = type;
		(*args)->argtypmods[index] = typmod;
		(*args)->argnames[index] = name;
		(*args)->argmodes[index] = mode;
	}

	if (isnull)
		(*fcinfo)->args[index + 2].value = (Datum) 0;
	else
		(*fcinfo)->args[index + 2].value = value;

	(*fcinfo)->args[index + 2].isnull = isnull;
	(*fcinfo)->nargs++;
	
	/* Safety check */
	if ((*fcinfo)->nargs > FUNC_MAX_ARGS)
		ereport(ERROR, (errcode(ERRCODE_TOO_MANY_ARGUMENTS),
				errmsg("cannot pass more than %d arguments to a procedure",
				       FUNC_MAX_ARGS)));
}

/*
 * Read out param API
 *
 * This function deconstruct the input composite Datum comp_value, and store the
 * info in values and nulls.
 */
void pltsql_read_composite_out_param(Datum comp_value, Datum **values, bool **nulls)
{
	HeapTupleData	tmptup;
	TupleDesc	tupdesc;
	HeapTupleHeader td;
	Oid		tupType;
	int32		tupTypmod;

	/* Get tuple body (note this could involve detoasting) */
	td = DatumGetHeapTupleHeader(comp_value);

	/* Build a temporary HeapTuple control structure */
	tmptup.t_len = HeapTupleHeaderGetDatumLength(td);
	ItemPointerSetInvalid(&(tmptup.t_self));
	tmptup.t_tableOid = InvalidOid;
	tmptup.t_data = td;

	/* Extract rowtype info and find a tupdesc */
	tupType = HeapTupleHeaderGetTypeId(td);
	tupTypmod = HeapTupleHeaderGetTypMod(td);
	tupdesc = lookup_rowtype_tupdesc(tupType, tupTypmod);


	if (tupdesc && HeapTupleIsValid(&tmptup))
	{
		int	td_natts = tupdesc->natts;

		*values = (Datum *) palloc(sizeof(Datum) * td_natts);
		*nulls = (bool *) palloc(sizeof(bool) * td_natts);

		heap_deform_tuple(&tmptup, tupdesc, *values, *nulls);
	}
	else
	{
		*values = NULL;
		*nulls = NULL;
	}
	ReleaseTupleDesc(tupdesc);
}

bool pltsql_suppress_string_truncation_error()
{
	return suppress_string_truncation_error;
}

void pltsql_read_procedure_info(StringInfo inout_str,
								bool *is_proc,
								Oid *typid,
								Oid *typmod,
								int *collation)
{
	Oid						func_oid = InvalidOid;
	Oid						atttypid;
	Oid						atttypmod;
	int						attcollation;
	bool					isStoredProcedure = true;
	HeapTuple				proctup = NULL;
	Form_pg_proc			proc = NULL;
	List	    *parsetree;
	CallStmt *cstmt;
	FuncCall *funccall;
	FuncCandidateList clist;
	const char  *str1 = "EXECUTE ";
	StringInfoData	    proc_stmt;

	/*
	 * Create a fake EXECUTE statement to get the function name
	 */
	initStringInfo(&proc_stmt);
	appendStringInfoString(&proc_stmt, str1);
	appendStringInfoString(&proc_stmt, inout_str->data);
	parsetree = raw_parser(proc_stmt.data, RAW_PARSE_DEFAULT);
	cstmt  = (CallStmt *) ((RawStmt *) linitial(parsetree))->stmt;

	funccall = cstmt->funccall;

	/*
	 * Parse the name into components and see if it matches any
	 * pg_proc entries in the current search path.
	 */
	clist = FuncnameGetCandidates(funccall->funcname, -1, NIL, false, false, false, false);

	if (clist == NULL)
	{
		/*
		 * We don't store some system procedures in the catalog, ex: sp_executesql,
		 * sp_prepare etc.  We can add a check for them here.  But, let's skip
		 * the check from here because when we're going to execute the procedure,
		 * if it doesn't exist or it's not a system procedure, then anywaay
		 * we're going to throw an error.
		 */
		isStoredProcedure = true;
	}
	else
	{
		if (clist->next != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_AMBIGUOUS_FUNCTION),
				 errmsg("more than one function named \"%s\"",
						NameListToString(funccall->funcname))));

		func_oid = clist->oid;
		Assert(func_oid != InvalidOid);

		/* Look up the function */
		proctup = SearchSysCache1(PROCOID, ObjectIdGetDatum(func_oid));

		/* shouldn't happen, otherwise regprocin would've thrown error */
		if (!HeapTupleIsValid(proctup))
			elog(ERROR, "cache lookup failed for function %d", func_oid);

		proc = (Form_pg_proc) GETSTRUCT(proctup);

		isStoredProcedure = (proc->prokind == PROKIND_PROCEDURE);
	}

	if (isStoredProcedure)
	{
		/* a procedure always returns integer */
		atttypid = INT4OID;
		atttypmod = -1;
		attcollation = -1;
	}
	else
	{
		Type        retType;
		Form_pg_type typtup;

		if (proc->proretset)
			ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("The request for procedure \"%s\" failed because \"%s\" is"
					   "a SET-returning function", NameStr(proc->proname),
					   NameStr(proc->proname))));

		if (proc->prorettype == RECORDOID || proc->prorettype == VOIDOID)
			ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("The request for procedure \"%s\" failed because \"%s\" is"
					   "not a scalar-valued function", NameStr(proc->proname),
					   NameStr(proc->proname))));

		retType = typeidType(proc->prorettype);
		typtup = (Form_pg_type) GETSTRUCT(retType);

		atttypid = proc->prorettype;
		attcollation = typtup->typcollation;

		/*
		 * By default, PG ignores the typmod of return type, so just pick th
		 * return type from the pg_type.  In [BABEL-1000], we've fixed this
		 * issue by storing the typemod of return type in pg_proc->probin
		 * field.  So, let's read the typmod from the same.
		 */
		Assert(func_oid != InvalidOid);
		atttypmod = probin_read_ret_typmod(func_oid, proc->pronargs, proc->prorettype);

		ReleaseSysCache((HeapTuple) retType);
	}

	if (proctup != NULL)
		ReleaseSysCache(proctup);

	if (is_proc)
		*is_proc = isStoredProcedure;
	if (typid)
		*typid = atttypid;
	if (typmod)
		*typmod = atttypmod;
	if (collation)
		*collation = attcollation;
}

void
PLTsqlStartTransaction(char *txnName)
{
	elog(DEBUG2, "TSQL TXN Start transaction %d", NestedTranCount);
	if (!IsTransactionBlockActive())
	{
		Assert(NestedTranCount == 0);
		BeginTransactionBlock();
		/*
		 * set transaction name in savepoint field.
		 * It is needed to distinguish rollback vs
		 * rollback to savepoint requests.
		 */
		if (txnName != NULL)
			SetTopTransactionName(txnName);
	}
	++NestedTranCount;

	if (*pltsql_protocol_plugin_ptr && (*pltsql_protocol_plugin_ptr)->set_at_at_stat_var)
			(*pltsql_protocol_plugin_ptr)->set_at_at_stat_var("trancount", NestedTranCount, 0);
}

void
PLTsqlCommitTransaction(QueryCompletion *qc, bool chain)
{
	elog(DEBUG2, "TSQL TXN Commit transaction %d", NestedTranCount);
	if (NestedTranCount <= 1)
	{
		RequireTransactionBlock(true, "COMMIT");
		if (!EndTransactionBlock(chain))
		{
			/* report unsuccessful commit in QueryCompletion */
			if (qc)
				qc->commandTag = CMDTAG_ROLLBACK;
		}
		NestedTranCount = 0;
	}
	else
	{
		--NestedTranCount;
	}

	if (*pltsql_protocol_plugin_ptr && (*pltsql_protocol_plugin_ptr)->set_at_at_stat_var)
			(*pltsql_protocol_plugin_ptr)->set_at_at_stat_var("trancount", NestedTranCount, 0);
}

void
PLTsqlRollbackTransaction(char *txnName, QueryCompletion *qc, bool chain)
{
	if (IsTopTransactionName(txnName))
	{
		elog(DEBUG2, "TSQL TXN Rollback transaction");
		RequireTransactionBlock(true, "ROLLBACK");
		/* Rollback request */
		UserAbortTransactionBlock(chain);
		NestedTranCount = 0;

		if (*pltsql_protocol_plugin_ptr && (*pltsql_protocol_plugin_ptr)->set_at_at_stat_var)
			(*pltsql_protocol_plugin_ptr)->set_at_at_stat_var("trancount", NestedTranCount, 0);
	}
	else
	{
		elog(DEBUG2, "TSQL TXN Rollback to savepoint %s", txnName);
		RequireTransactionBlock(true, "ROLLBACK TO SAVEPOINT");
		RollbackToSavepoint(txnName);
		RollbackAndReleaseSavepoint(txnName);
		if (qc)
			//			strncpy(completionTag, "ROLLBACK TO SAVEPOINT");
			/* PG 13 merge: double check this line */
			qc->commandTag = CMDTAG_SAVEPOINT;
	}
}

void
pltsql_start_txn(void)
{
	PLTsqlStartTransaction(NULL);
	CommitTransactionCommand();
}

void
pltsql_commit_txn(void)
{
	PLTsqlCommitTransaction(NULL, false);
	CommitTransactionCommand();
	StartTransactionCommand();
}

void
pltsql_rollback_txn(void)
{
	PLTsqlRollbackTransaction(NULL, NULL, false);
	CommitTransactionCommand();
	StartTransactionCommand();
}

bool
pltsql_get_errdata(int *tsql_error_code, int *tsql_error_severity, int *tsql_error_state)
{
	if (exec_state_call_stack == NULL ||
		exec_state_call_stack->error_data.error_number < 50000)
		return false;

	if (tsql_error_code)
		*tsql_error_code = exec_state_call_stack->error_data.error_number;
	if (tsql_error_severity)
		*tsql_error_severity = exec_state_call_stack->error_data.error_severity;
	if (tsql_error_state)
		*tsql_error_state = exec_state_call_stack->error_data.error_state;
	return true;
}

bool
is_sysname_column(ColumnDef *coldef)
{
	return pg_strcasecmp(((Value *) llast(coldef->typeName->names))->val.str, "sysname") == 0;
}

bool
have_null_constr(List *constr_list)
{
	ListCell *lc;
	bool isnull = false;

	foreach(lc, constr_list)
	{
		Constraint *c = lfirst_node(Constraint, lc);
		if (c->contype == CONSTR_NULL)
		{
			isnull = true;
			break;
		}
	}
	return isnull;
}

Node *
parsetree_nth_stmt(List *parsetree, int n)
{
	return ((RawStmt *) list_nth(parsetree, n))->stmt;
}

/* 
 * Functions to update parsed dummy statements with real values 
 */
void
update_AlterTableStmt(Node *n, const char *tbl_schema, const char *newowner)
{
	AlterTableStmt *stmt = (AlterTableStmt *) n;
	ListCell *lc;
	if (!IsA(stmt, AlterTableStmt))
		ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR), errmsg("query is not a AlterTableStmt")));

	if (tbl_schema)
		stmt->relation->schemaname = pstrdup(tbl_schema);

	if (!newowner)
		return;

	foreach(lc, stmt->cmds)
	{
		AlterTableCmd *cmd = (AlterTableCmd *) lfirst(lc);
		switch (cmd->subtype)
		{
			case AT_ChangeOwner:
			{
				cmd->newowner->rolename = pstrdup(newowner);
				break;
			}
			default:
				break;
		}
	}
}

void
update_CreateRoleStmt(Node *n, const char *role, const char *member, const char *addto)
{
	CreateRoleStmt *stmt = (CreateRoleStmt *) n;
	ListCell *option;
	if (!IsA(stmt, CreateRoleStmt))
		ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR), errmsg("query is not a CreateRoleStmt")));

	if (role)
		stmt->role = pstrdup(role);

	if (!member && !addto)
		return;

	foreach(option, stmt->options)
	{
		DefElem *defel = (DefElem *) lfirst(option);

		if (member && defel->arg && strcmp(defel->defname, "rolemembers") == 0)
		{
			RoleSpec *tmp = (RoleSpec *) llast((List *) defel->arg);
			tmp->rolename = pstrdup(member);
		}
		else if (addto && defel->arg && strcmp(defel->defname, "addroleto") == 0)
		{
			RoleSpec *tmp = (RoleSpec *) llast((List *) defel->arg);
			tmp->rolename = pstrdup(addto);
		}
	}
}

void
update_AlterRoleStmt(Node *n, RoleSpec *role)
{
	AlterRoleStmt *stmt = (AlterRoleStmt *) n;
	if (!IsA(stmt, AlterRoleStmt))
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("query is not an AlterRoleStmt")));

	stmt->role = role;
}

void
update_CreateSchemaStmt(Node *n, const char *schemaname, const char *authrole)
{
	CreateSchemaStmt *stmt = (CreateSchemaStmt *) n;
	if (!IsA(stmt, CreateSchemaStmt))
		ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR), errmsg("query is not a CreateSchemaStmt")));

	if (schemaname)
		stmt->schemaname = pstrdup(schemaname);

	if (authrole)
		stmt->authrole->rolename = pstrdup(authrole);
}

void
update_DropOwnedStmt(Node *n, List *role_list)
{
	DropOwnedStmt	*stmt = (DropOwnedStmt *) n;
	List			*rolespec_list = NIL;
	ListCell		*elem;

	if (!IsA(stmt, DropOwnedStmt))
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("query is not a DropOwnedStmt")));

	foreach (elem, role_list)
	{
		char *name = (char *) lfirst(elem);
		RoleSpec *tmp = makeNode(RoleSpec);

		tmp->roletype = ROLESPEC_CSTRING;
		tmp->location = -1;
		tmp->rolename = pstrdup(name);
		rolespec_list = lappend(rolespec_list, tmp);
	}
	stmt->roles = rolespec_list;
}

void
update_DropRoleStmt(Node *n, const char *role)
{
	DropRoleStmt *stmt = (DropRoleStmt *) n;
	if (!IsA(stmt, DropRoleStmt))
		ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR), errmsg("query is not a DropRoleStmt")));

	if (role && stmt->roles)
	{
		/* 
		 * Delete the first element if it's is_role flag, in this way we won't
		 * need to rewrite the role names during internal call.
		 */
		RoleSpec *tmp = (RoleSpec *) linitial(stmt->roles);

		if (strcmp(tmp->rolename, "is_role") == 0)
			stmt->roles = list_delete_cell(stmt->roles, list_head(stmt->roles));

		pfree(tmp);

		if (!stmt->roles)
			return;

		/* Update the statement with given role name */
		tmp = (RoleSpec *) llast(stmt->roles);
		tmp->rolename = pstrdup(role);
	}
}

void
update_DropStmt(Node *n, const char *object)
{
	DropStmt *stmt = (DropStmt *) n;
	if (!IsA(stmt, DropStmt))
		ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR), errmsg("query is not a DropStmt")));

	if (object && stmt->objects)
		llast(stmt->objects) = makeString(pstrdup(object));
}

void
update_GrantRoleStmt(Node *n, List *privs, List *roles)
{
	GrantRoleStmt *stmt = (GrantRoleStmt *) n;
	if (!IsA(stmt, GrantRoleStmt))
		ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR), errmsg("query is not a GrantRoleStmt")));

	stmt->granted_roles = privs;
	stmt->grantee_roles = roles;
}

void
update_GrantStmt(Node *n, const char *object, const char *obj_schema, const char *grantee)
{
	GrantStmt *stmt = (GrantStmt *) n;
	if (!IsA(stmt, GrantStmt))
		ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR), errmsg("query is not a GrantStmt")));

	if (object && stmt->objects)
		llast(stmt->objects) = makeString(pstrdup(object));
	else if (obj_schema && stmt->objects)
	{
		RangeVar *tmp = (RangeVar *) llast(stmt->objects);
		tmp->schemaname = pstrdup(obj_schema);
	}

	if (grantee && stmt->grantees)
	{
		RoleSpec *tmp = (RoleSpec *) llast(stmt->grantees);
		tmp->rolename = pstrdup(grantee);
	}
}

void
update_RenameStmt(Node *n, const char *old_name, const char *new_name)
{
	RenameStmt *stmt = (RenameStmt *) n;
	if (!IsA(stmt, RenameStmt))
		ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR), errmsg("query is not a RenameStmt")));

	stmt->subname = pstrdup(old_name);
	stmt->newname = pstrdup(new_name);
}

void
update_ViewStmt(Node *n, const char *view_schema)
{
	ViewStmt *stmt = (ViewStmt *) n;
	if (!IsA(stmt, ViewStmt))
		ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR), errmsg("query is not a ViewStmt")));

	if (view_schema)
		stmt->view->schemaname = pstrdup(view_schema);
}

bool is_tsql_any_char_datatype(Oid oid)
{
	return is_tsql_bpchar_datatype(oid) ||
		is_tsql_nchar_datatype(oid) ||
		is_tsql_varchar_datatype(oid) ||
		is_tsql_nvarchar_datatype(oid);
}

bool is_tsql_text_ntext_or_image_datatype(Oid oid)
{
	return is_tsql_text_datatype(oid) ||
		is_tsql_ntext_datatype(oid) ||
		is_tsql_image_datatype(oid);
}

/*
 * Try to acquire a lock with no wait
 */
bool
TryLockLogicalDatabaseForSession(int16 dbid, LOCKMODE lockmode)
{
	LOCKTAG tag;

	SET_LOCKTAG_INT16(tag, dbid); 

	return LockAcquire(&tag, lockmode, true, true) != LOCKACQUIRE_NOT_AVAIL;
}

/*
 * Release the lock
 */
void
UnlockLogicalDatabaseForSession(int16 dbid, LOCKMODE lockmode, bool force)
{
	LOCKTAG tag;

	SET_LOCKTAG_INT16(tag, dbid);

	if (!force && !LockHeldByMe(&tag, lockmode))
		return;

	LockRelease(&tag, lockmode, true);
}

/*
 * Converts a BpChar (TSQL CHAR(n)) type to cstring
 */
char *
bpchar_to_cstring(const BpChar *bpchar)
{
	const char *bp_data = VARDATA_ANY(bpchar);
	int len = VARSIZE_ANY_EXHDR(bpchar);

	char *result = (char *) palloc(len + 1);
	memcpy(result, bp_data, len);
	result[len] = '\0';

	return result;
}

/*
 * Converts a VarChar type to cstring
 */
char *
varchar_to_cstring(const VarChar *varchar)
{
	const char *vc_data = VARDATA_ANY(varchar);
	int len = VARSIZE_ANY_EXHDR(varchar);

	char *result = (char *) palloc(len + 1);
	memcpy(result, vc_data, len);
	result[len] = '\0';

	return result;
}

/*
 * Convert list of schema OIDs to schema names.
 */

char *
flatten_search_path(List *oid_list)
{
	StringInfoData pathbuf;
	ListCell   *lc;

	initStringInfo(&pathbuf);

	foreach(lc, oid_list)
	{
		Oid			schema_oid = lfirst_oid(lc);
		char	   *schema_name = get_namespace_name(schema_oid);
		appendStringInfo(&pathbuf, " %s,", quote_identifier(schema_name));
	}
	pathbuf.data[strlen(pathbuf.data) - 1] = '\0';
	return pathbuf.data;
}

const char *
get_pltsql_function_signature_internal(const char *funcname,
							  int nargs, const Oid *argtypes)
{
	StringInfoData argbuf;
	int			i;
	const char *prev_quote_ident = GetConfigOption("quote_all_identifiers", true, true);

	initStringInfo(&argbuf);

	PG_TRY();
	{
		/* Temporarily set quote_all_identifiers to TRUE to generate quoted string */
		set_config_option("quote_all_identifiers", "true",
								(superuser() ? PGC_SUSET : PGC_USERSET),
								PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);

		appendStringInfo(&argbuf, "%s(", funcname);
		for (i = 0; i < nargs; i++)
		{
			if (i)
				appendStringInfoString(&argbuf, ", ");
			appendStringInfoString(&argbuf, format_type_be_qualified(argtypes[i]));
		}
		appendStringInfoChar(&argbuf, ')');
	}
	PG_FINALLY();
	{
		set_config_option("quote_all_identifiers", prev_quote_ident,
								(superuser() ? PGC_SUSET : PGC_USERSET),
								PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);
	}
	PG_END_TRY();

	return argbuf.data;			/* return palloc'd string buffer */
}

PG_FUNCTION_INFO_V1(get_pltsql_function_signature);

Datum
get_pltsql_function_signature(PG_FUNCTION_ARGS)
{
	Oid			funcoid = PG_GETARG_OID(0);
	HeapTuple	proctup;
	Form_pg_proc form_proctup;
	char		*func_signature;

	proctup = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcoid));
	if (!HeapTupleIsValid(proctup))
		elog(ERROR, "cache lookup failed for function %u", funcoid);
	form_proctup = (Form_pg_proc) GETSTRUCT(proctup);

	func_signature = get_pltsql_function_signature_internal(NameStr(form_proctup->proname),
															form_proctup->pronargs,
															form_proctup->proargtypes.values);

	ReleaseSysCache(proctup);
	PG_RETURN_TEXT_P(cstring_to_text(func_signature));
}
