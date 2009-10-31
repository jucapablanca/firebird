/*
 * The contents of this file are subject to the Interbase Public
 * License Version 1.0 (the "License"); you may not use this file
 * except in compliance with the License. You may obtain a copy
 * of the License at http://www.Inprise.com/IPL.html
 *
 * Software distributed under the License is distributed on an
 * "AS IS" basis, WITHOUT WARRANTY OF ANY KIND, either express
 * or implied. See the License for the specific language governing
 * rights and limitations under the License.
 *
 * The Original Code was created by Inprise Corporation
 * and its predecessors. Portions created by Inprise Corporation are
 * Copyright (C) Inprise Corporation.
 *
 * All Rights Reserved.
 * Contributor(s): ______________________________________.
 * Adriano dos Santos Fernandes
 */

#include "firebird.h"
#include "../jrd/jrd.h"
#include "../jrd/ini.h"
#include "../jrd/rse.h"
#include "../jrd/exe_proto.h"
#include "../jrd/opt_proto.h"
#include "../jrd/rse_proto.h"
#include "../jrd/blb_proto.h"
#include "../jrd/evl_proto.h"
#include "../jrd/intl_proto.h"
#include "../jrd/mov_proto.h"
#include "../jrd/sort_proto.h"
#include "../jrd/vio_proto.h"
#include "../jrd/AggregateRsb.h"

using namespace Jrd;
using namespace Firebird;


static bool rejectDuplicate(const UCHAR*, const UCHAR*, void*);


// Callback routine used by sort/project to reject duplicate values.
// Particularly dumb routine -- always returns true;
static bool rejectDuplicate(const UCHAR* /*data1*/, const UCHAR* /*data2*/, void* /*userArg*/)
{
	return true;
}


AggregateRsb::AggregateRsb(RecordSource* aRsb, jrd_nod* aAggNode)
	: RecordStream(aRsb),
	  aggNode(aAggNode),
	  next(rsb->rsb_next)
{
}


RecordSource* AggregateRsb::create(thread_db* tdbb, OptimizerBlk* opt, jrd_nod* node,
	NodeStack& deliverStack, RecordSelExpr* rse)
{
	SET_TDBB(tdbb);

	CompilerScratch* csb = opt->opt_csb;

	RecordSource* rsb = FB_NEW_RPT(*tdbb->getDefaultPool(), 0) RecordSource;
	rsb->rsb_type = rsb_record_stream;
	rsb->rsb_stream = (UCHAR) (IPTR) node->nod_arg[e_agg_stream];
	rsb->rsb_format = csb->csb_rpt[rsb->rsb_stream].csb_format;
	rsb->rsb_next = OPT_compile(tdbb, csb, rse, &deliverStack);
	rsb->rsb_impure = CMP_impure(csb, sizeof(irsb));
	rsb->rsb_record_stream = FB_NEW(*tdbb->getDefaultPool()) AggregateRsb(rsb, node);

	return rsb;
}


void AggregateRsb::findRsbs(StreamStack* streamList, RsbStack* rsbList)
{
	RecordStream::findRsbs(streamList, rsbList);

	if (rsbList)
		rsbList->push(rsb);
}


void AggregateRsb::invalidate(thread_db* tdbb, record_param* rpb)
{
	RecordStream::invalidate(tdbb, rpb);
	RSE_invalidate_child_rpbs(tdbb, next);
}


unsigned AggregateRsb::dump(UCHAR* buffer, unsigned bufferLen)
{
	UCHAR* bufferStart = buffer;

	if (bufferLen > 0)
		*buffer++ = isc_info_rsb_aggregate;

	return buffer - bufferStart;
}


void AggregateRsb::open(thread_db* tdbb, jrd_req* request)
{
	SET_TDBB(tdbb);

	record_param* const rpb = &request->req_rpb[rsb->rsb_stream];
	irsb* impure = (irsb*) ((UCHAR*) request + rsb->rsb_impure);

	impure->irsb_count = 3;

	VIO_record(tdbb, rpb, rsb->rsb_format, tdbb->getDefaultPool());
}


void AggregateRsb::close(thread_db* tdbb)
{
	SET_TDBB(tdbb);

	RSE_close(tdbb, next);
}


bool AggregateRsb::get(thread_db* tdbb, jrd_req* request)
{
	SET_TDBB(tdbb);

	irsb* impure = (irsb*) ((UCHAR*) request + rsb->rsb_impure);

	if ((impure->irsb_count = evlGroup(tdbb, request, impure->irsb_count)))
		return true;
	else
		return false;
}


void AggregateRsb::markRecursive()
{
	OPT_mark_rsb_recursive(next);
}


// Compute the next aggregated record of a value group. evlGroup is driven by, and returns, a state
// variable. The values of the state are:
//
// 3  Entering EVL group beforing fetching the first record.
// 1  Values are pending from a prior fetch
// 2  We encountered EOF from the last attempted fetch
// 0  We processed everything now process (EOF)
USHORT AggregateRsb::evlGroup(thread_db* tdbb, jrd_req* request, USHORT state)
{
	SET_TDBB(tdbb);

	if (--tdbb->tdbb_quantum < 0)
		JRD_reschedule(tdbb, 0, true);

	impure_value vtemp;
	vtemp.vlu_string = NULL;

	jrd_nod* map = aggNode->nod_arg[e_agg_map];
	jrd_nod* group = aggNode->nod_arg[e_agg_group];

	jrd_nod** ptr;
	const jrd_nod* const* end;

	// if we found the last record last time, we're all done

	if (state == 2)
		return 0;

	try
	{
		// Initialize the aggregate record

		for (ptr = map->nod_arg, end = ptr + map->nod_count; ptr < end; ptr++)
		{
			const jrd_nod* from = (*ptr)->nod_arg[e_asgn_from];
			impure_value_ex* impure = (impure_value_ex*) ((SCHAR*) request + from->nod_impure);
			impure->vlux_count = 0;

			switch (from->nod_type)
			{
			case nod_agg_average:
			case nod_agg_average_distinct:
				impure->vlu_desc.dsc_dtype = DEFAULT_DOUBLE;
				impure->vlu_desc.dsc_length = sizeof(double);
				impure->vlu_desc.dsc_sub_type = 0;
				impure->vlu_desc.dsc_scale = 0;
				impure->vlu_desc.dsc_address = (UCHAR*) &impure->vlu_misc.vlu_double;
				impure->vlu_misc.vlu_double = 0;
				if (from->nod_type == nod_agg_average_distinct)
					// Initialize a sort to reject duplicate values
					initDistinct(request, from);
				break;

			case nod_agg_average2:
			case nod_agg_average_distinct2:
				// Initialize the result area as an int64.  If the field being
				// averaged is approximate numeric, the first call to add2 will
				// convert the descriptor to double.
				impure->make_int64(0, from->nod_scale);
				if (from->nod_type == nod_agg_average_distinct2)
					/* Initialize a sort to reject duplicate values */
					initDistinct(request, from);
				break;

			case nod_agg_total:
			case nod_agg_total_distinct:
				impure->make_long(0);
				if (from->nod_type == nod_agg_total_distinct)
					// Initialize a sort to reject duplicate values
					initDistinct(request, from);
				break;

			case nod_agg_total2:
			case nod_agg_total_distinct2:
				// Initialize the result area as an int64.  If the field being
				// averaged is approximate numeric, the first call to add2 will
				// convert the descriptor to double.
				impure->make_int64(0, from->nod_scale);
				if (from->nod_type == nod_agg_total_distinct2)
					// Initialize a sort to reject duplicate values
					initDistinct(request, from);
				break;

			case nod_agg_min:
			case nod_agg_min_indexed:
			case nod_agg_max:
			case nod_agg_max_indexed:
				impure->vlu_desc.dsc_dtype = 0;
				break;

			case nod_agg_count:
			case nod_agg_count2:
			case nod_agg_count_distinct:
				impure->make_long(0);
				if (from->nod_type == nod_agg_count_distinct)
					// Initialize a sort to reject duplicate values
					initDistinct(request, from);
				break;

			case nod_agg_list:
			case nod_agg_list_distinct:
				// We don't know here what should be the sub-type and text-type.
				// Defer blob creation for when first record is found.
				impure->vlu_blob = NULL;
				impure->vlu_desc.dsc_dtype = 0;

				if (from->nod_type == nod_agg_list_distinct)
					// Initialize a sort to reject duplicate values
					initDistinct(request, from);
				break;

			case nod_literal:	// pjpg 20001124
				EXE_assignment(tdbb, *ptr);
				break;

			default:    // Shut up some compiler warnings
				break;
			}
		}

		// If there isn't a record pending, open the stream and get one

		if (state == 0 || state == 3)
		{
			RSE_open(tdbb, next);
			if (!RSE_get_record(tdbb, next))
			{
				if (group)
				{
					finiDistinct(request, aggNode);
					return 0;
				}
				state = 2;
			}
		}

		dsc* desc;

		if (group)
		{
			for (ptr = group->nod_arg, end = ptr + group->nod_count; ptr < end; ptr++)
			{
				jrd_nod* from = *ptr;
				impure_value_ex* impure = (impure_value_ex*) ((SCHAR*) request + from->nod_impure);
				desc = EVL_expr(tdbb, from);
				if (request->req_flags & req_null)
					impure->vlu_desc.dsc_address = NULL;
				else
					EVL_make_value(tdbb, desc, impure);
			}
		}

		// Loop thru records until either a value change or EOF

		bool first = true;

		while (state != 2)
		{
			state = 1;

			if (first)
				first = false;
			else
			{
				// In the case of a group by, look for a change in value of any of
				// the columns; if we find one, stop aggregating and return what we have.

				if (group)
				{
					for (ptr = group->nod_arg, end = ptr + group->nod_count; ptr < end; ptr++)
					{
						jrd_nod* from = *ptr;
						impure_value_ex* impure = (impure_value_ex*) ((SCHAR*) request + from->nod_impure);

						if (impure->vlu_desc.dsc_address)
							EVL_make_value(tdbb, &impure->vlu_desc, &vtemp);
						else
							vtemp.vlu_desc.dsc_address = NULL;

						desc = EVL_expr(tdbb, from);

						if (request->req_flags & req_null)
						{
							impure->vlu_desc.dsc_address = NULL;
							if (vtemp.vlu_desc.dsc_address)
								goto break_out;
						}
						else
						{
							EVL_make_value(tdbb, desc, impure);
							if (!vtemp.vlu_desc.dsc_address || MOV_compare(&vtemp.vlu_desc, desc))
								goto break_out;
						}
					}
				}
			}

			// go through and compute all the aggregates on this record

			for (ptr = map->nod_arg, end = ptr + map->nod_count; ptr < end; ptr++)
			{
				jrd_nod* from = (*ptr)->nod_arg[e_asgn_from];
				impure_value_ex* impure = (impure_value_ex*) ((SCHAR*) request + from->nod_impure);

				switch (from->nod_type)
				{
				case nod_agg_min:
				case nod_agg_min_indexed:
				case nod_agg_max:
				case nod_agg_max_indexed:
					{
						desc = EVL_expr(tdbb, from->nod_arg[0]);
						if (request->req_flags & req_null)
							break;

						// if a max or min has been mapped to an index,
						// then the first record is the EOF

						if (from->nod_type == nod_agg_max_indexed || from->nod_type == nod_agg_min_indexed)
							state = 2;

						++impure->vlux_count;
						if (!impure->vlu_desc.dsc_dtype)
						{
							EVL_make_value(tdbb, desc, impure);
							// It was reinterpret_cast<impure_value*>(&impure->vlu_desc));
							// but vlu_desc is the first member of impure_value and impure_value_ex
							// derives from impure_value and impure_value doesn't derive from anything
							// and it doesn't contain virtuals.
							// Thus, &impure_value_ex->vlu_desc == &impure_value->vlu_desc == &impure_value_ex
							// Delete this comment or restore the original code
							// when this reasoning has been validated, please.
							break;
						}

						const SLONG result = MOV_compare(desc, reinterpret_cast<dsc*>(impure));

						if ((result > 0 &&
								(from->nod_type == nod_agg_max || from->nod_type == nod_agg_max_indexed)) ||
							(result < 0 &&
								(from->nod_type == nod_agg_min || from->nod_type == nod_agg_min_indexed)))
						{
							EVL_make_value(tdbb, desc, impure);
						}

						break;
					}

				case nod_agg_total:
				case nod_agg_average:
					desc = EVL_expr(tdbb, from->nod_arg[0]);
					if (request->req_flags & req_null)
						break;
					++impure->vlux_count;
					EVL_add(desc, from, impure);
					break;

				case nod_agg_total2:
				case nod_agg_average2:
					desc = EVL_expr(tdbb, from->nod_arg[0]);
					if (request->req_flags & req_null)
						break;
					++impure->vlux_count;
					EVL_add2(desc, from, impure);
					break;

				case nod_agg_count2:
					++impure->vlux_count;
					desc = EVL_expr(tdbb, from->nod_arg[0]);
					if (request->req_flags & req_null)
						break;

				case nod_agg_count:
					++impure->vlux_count;
					++impure->vlu_misc.vlu_long;
					break;

				case nod_agg_list:
					{
						MoveBuffer buffer;
						UCHAR* temp;
						int len;

						desc = EVL_expr(tdbb, from->nod_arg[0]);
						if (request->req_flags & req_null)
							break;

						if (!impure->vlu_blob)
						{
							impure->vlu_blob = BLB_create(tdbb, request->req_transaction,
								&impure->vlu_misc.vlu_bid);
							impure->vlu_desc.makeBlob(desc->getBlobSubType(), desc->getTextType(),
								(ISC_QUAD*) &impure->vlu_misc.vlu_bid);
						}

						if (impure->vlux_count)
						{
							const dsc* const delimiter = EVL_expr(tdbb, from->nod_arg[1]);
							if (request->req_flags & req_null)
							{
								// mark the result as NULL
								impure->vlu_desc.dsc_dtype = 0;
								break;
							}

							len = MOV_make_string2(tdbb, delimiter, impure->vlu_desc.getTextType(),
								&temp, buffer, false);
							BLB_put_data(tdbb, impure->vlu_blob, temp, len);
						}
						++impure->vlux_count;
						len = MOV_make_string2(tdbb, desc, impure->vlu_desc.getTextType(),
							&temp, buffer, false);
						BLB_put_data(tdbb, impure->vlu_blob, temp, len);
						break;
					}

				case nod_agg_count_distinct:
				case nod_agg_total_distinct:
				case nod_agg_average_distinct:
				case nod_agg_average_distinct2:
				case nod_agg_total_distinct2:
				case nod_agg_list_distinct:
					{
						desc = EVL_expr(tdbb, from->nod_arg[0]);
						if (request->req_flags & req_null)
							break;
						// "Put" the value to sort.
						const size_t asbIndex = (from->nod_type == nod_agg_list_distinct) ? 2 : 1;
						AggregateSort* asb = (AggregateSort*) from->nod_arg[asbIndex];
						impure_agg_sort* asbImpure =
							(impure_agg_sort*) ((SCHAR*) request + asb->nod_impure);
						UCHAR* data;
						SORT_put(tdbb, asbImpure->iasb_sort_handle, reinterpret_cast<ULONG**>(&data));

						MOVE_CLEAR(data, asb->asb_length);

						if (asb->asb_intl)
						{
							// convert to an international byte array
							dsc to;
							to.dsc_dtype = dtype_text;
							to.dsc_flags = 0;
							to.dsc_sub_type = 0;
							to.dsc_scale = 0;
							to.dsc_ttype() = ttype_sort_key;
							to.dsc_length = asb->asb_key_desc[0].skd_length;
							to.dsc_address = data;
							INTL_string_to_key(tdbb, INTL_TEXT_TO_INDEX(desc->getTextType()),
								desc, &to, INTL_KEY_UNIQUE);
						}

						asb->asb_desc.dsc_address = data +
							(asb->asb_intl ? asb->asb_key_desc[1].skd_offset : 0);
						MOV_move(tdbb, desc, &asb->asb_desc);

						break;
					}

				default:
					EXE_assignment(tdbb, *ptr);
				}
			}

			if (state == 2)
				break;

			if (!RSE_get_record(tdbb, next))
				state = 2;
		}

		break_out:

		for (ptr = map->nod_arg, end = ptr + map->nod_count; ptr < end; ptr++)
		{
			const jrd_nod* from = (*ptr)->nod_arg[e_asgn_from];
			impure_value_ex* impure = (impure_value_ex*) ((SCHAR*) request + from->nod_impure);

			if (from->nod_type == nod_agg_list && impure->vlu_blob)
			{
				BLB_close(tdbb, impure->vlu_blob);
				impure->vlu_blob = NULL;
			}
		}

		// Finish up any residual computations and get out

		delete vtemp.vlu_string;

		dsc temp;
		double d;
		SINT64 i;

		for (ptr = map->nod_arg, end = ptr + map->nod_count; ptr < end; ptr++)
		{
			jrd_nod* from = (*ptr)->nod_arg[e_asgn_from];
			jrd_nod* field = (*ptr)->nod_arg[e_asgn_to];
			const USHORT id = (USHORT)(IPTR) field->nod_arg[e_fld_id];
			Record* record = request->req_rpb[(int) (IPTR) field->nod_arg[e_fld_stream]].rpb_record;
			impure_value_ex* impure = (impure_value_ex*) ((SCHAR*) request + from->nod_impure);

			switch (from->nod_type)
			{
			case nod_agg_min:
			case nod_agg_min_indexed:
			case nod_agg_max:
			case nod_agg_max_indexed:
			case nod_agg_total:
			case nod_agg_total_distinct:
			case nod_agg_total2:
			case nod_agg_total_distinct2:
			case nod_agg_list:
			case nod_agg_list_distinct:
				if (from->nod_type == nod_agg_total_distinct ||
					from->nod_type == nod_agg_total_distinct2 ||
					from->nod_type == nod_agg_list_distinct)
				{
					computeDistinct(tdbb, request, from);
				}

				if (!impure->vlux_count)
				{
					SET_NULL(record, id);
					break;
				}
				// If vlux_count is non-zero, we need to fall through.

			case nod_agg_count:
			case nod_agg_count2:
			case nod_agg_count_distinct:
				if (from->nod_type == nod_agg_count_distinct)
					computeDistinct(tdbb, request, from);

				if (!impure->vlu_desc.dsc_dtype)
					SET_NULL(record, id);
				else
				{
					MOV_move(tdbb, &impure->vlu_desc, EVL_assign_to(tdbb, field));
					CLEAR_NULL(record, id);
				}
				break;

			case nod_agg_average_distinct:
				computeDistinct(tdbb, request, from);
				// fall through

			case nod_agg_average:
				if (!impure->vlux_count)
				{
					SET_NULL(record, id);
					break;
				}
				CLEAR_NULL(record, id);
				temp.dsc_dtype = DEFAULT_DOUBLE;
				temp.dsc_length = sizeof(double);
				temp.dsc_scale = 0;
				temp.dsc_sub_type = 0;
				temp.dsc_address = (UCHAR*) &d;
				d = MOV_get_double(&impure->vlu_desc) / impure->vlux_count;
				MOV_move(tdbb, &temp, EVL_assign_to(tdbb, field));
				break;

			case nod_agg_average_distinct2:
				computeDistinct(tdbb, request, from);
				// fall through

			case nod_agg_average2:
				if (!impure->vlux_count)
				{
					SET_NULL(record, id);
					break;
				}

				CLEAR_NULL(record, id);
				temp.dsc_sub_type = 0;

				if (dtype_int64 == impure->vlu_desc.dsc_dtype)
				{
					temp.dsc_dtype = dtype_int64;
					temp.dsc_length = sizeof(SINT64);
					temp.dsc_scale = impure->vlu_desc.dsc_scale;
					temp.dsc_address = (UCHAR*) &i;
					i = *((SINT64 *) impure->vlu_desc.dsc_address) / impure->vlux_count;
				}
				else
				{
					temp.dsc_dtype = DEFAULT_DOUBLE;
					temp.dsc_length = sizeof(double);
					temp.dsc_scale = 0;
					temp.dsc_address = (UCHAR*) &d;
					d = MOV_get_double(&impure->vlu_desc) / impure->vlux_count;
				}

				MOV_move(tdbb, &temp, EVL_assign_to(tdbb, field));
				break;

			default:	// Shut up some compiler warnings
				break;
			}
		}
	}
	catch (const Exception& ex)
	{
		stuff_exception(tdbb->tdbb_status_vector, ex);
		finiDistinct(request, aggNode);
		ERR_punt();
	}

	return state;
}


// Initialize a sort for distinct aggregate.
void AggregateRsb::initDistinct(jrd_req* request, const jrd_nod* node)
{
	DEV_BLKCHK(node, type_nod);

	Database* database = request->req_attachment->att_database;

	const size_t asbIndex = (node->nod_type == nod_agg_list_distinct) ? 2 : 1;
	const AggregateSort* asb = (AggregateSort*) node->nod_arg[asbIndex];
	impure_agg_sort* asbImpure = (impure_agg_sort*) ((char*) request + asb->nod_impure);
	const sort_key_def* sortKey = asb->asb_key_desc;

	// Get rid of the old sort areas if this request has been used already
	SORT_fini(asbImpure->iasb_sort_handle);

	asbImpure->iasb_sort_handle = SORT_init(database, &request->req_sorts,
		ROUNDUP_LONG(asb->asb_length), (asb->asb_intl ? 2 : 1), 1, sortKey, rejectDuplicate, 0);
}


// Sort/project the values and compute the aggregate.
void AggregateRsb::computeDistinct(thread_db* tdbb, jrd_req* request, jrd_nod* node)
{
	SET_TDBB(tdbb);

	DEV_BLKCHK(node, type_nod);

	const size_t asbIndex = (node->nod_type == nod_agg_list_distinct) ? 2 : 1;
	AggregateSort* asb = (AggregateSort*) node->nod_arg[asbIndex];
	impure_agg_sort* asbImpure = (impure_agg_sort*) ((SCHAR*) request + asb->nod_impure);
	dsc* desc = &asb->asb_desc;
	impure_value_ex* impure = (impure_value_ex*) ((SCHAR*) request + node->nod_impure);

	// Sort the values already "put" to sort

	SORT_sort(tdbb, asbImpure->iasb_sort_handle);

	// Now get the sorted/projected values and compute the aggregate

	while (true)
	{
		UCHAR* data;
		SORT_get(tdbb, asbImpure->iasb_sort_handle, reinterpret_cast<ULONG**>(&data));

		if (data == NULL)
		{
			// we are done, close the sort
			SORT_fini(asbImpure->iasb_sort_handle);
			asbImpure->iasb_sort_handle = NULL;
			break;
		}

		desc->dsc_address = data + (asb->asb_intl ? asb->asb_key_desc[1].skd_offset : 0);

		switch (node->nod_type)
		{
		case nod_agg_total_distinct:
		case nod_agg_average_distinct:
			++impure->vlux_count;
			EVL_add(desc, node, impure);
			break;

		case nod_agg_total_distinct2:
		case nod_agg_average_distinct2:
			++impure->vlux_count;
			EVL_add2(desc, node, impure);
			break;

		case nod_agg_count_distinct:
			++impure->vlux_count;
			++impure->vlu_misc.vlu_long;
			break;

		case nod_agg_list_distinct:
			{
				if (!impure->vlu_blob)
				{
					impure->vlu_blob = BLB_create(tdbb, request->req_transaction,
						&impure->vlu_misc.vlu_bid);
					impure->vlu_desc.makeBlob(desc->getBlobSubType(), desc->getTextType(),
						(ISC_QUAD*) &impure->vlu_misc.vlu_bid);
				}

				MoveBuffer buffer;
				UCHAR* temp;
				int len;

				if (impure->vlux_count)
				{
					const dsc* const delimiter = EVL_expr(tdbb, node->nod_arg[1]);
					if (request->req_flags & req_null)
					{
						// mark the result as NULL
						impure->vlu_desc.dsc_dtype = 0;
						break;
					}

					len = MOV_make_string2(tdbb, delimiter, impure->vlu_desc.getTextType(), &temp,
						buffer, false);
					BLB_put_data(tdbb, impure->vlu_blob, temp, len);
				}

				++impure->vlux_count;
				len = MOV_make_string2(tdbb, desc, impure->vlu_desc.getTextType(), &temp, buffer, false);
				BLB_put_data(tdbb, impure->vlu_blob, temp, len);
				break;
			}

		default:	// Shut up some warnings
			break;
		}
	}

	if (node->nod_type == nod_agg_list_distinct && impure->vlu_blob)
	{
		BLB_close(tdbb, impure->vlu_blob);
		impure->vlu_blob = NULL;
	}
}


// Finalize a sort for distinct aggregate.
void AggregateRsb::finiDistinct(jrd_req* request, const jrd_nod* const node)
{
	DEV_BLKCHK(node, type_nod);

	jrd_nod* map = node->nod_arg[e_agg_map];

	jrd_nod** ptr;
	const jrd_nod* const* end;

	for (ptr = map->nod_arg, end = ptr + map->nod_count; ptr < end; ptr++)
	{
		const jrd_nod* from = (*ptr)->nod_arg[e_asgn_from];

		switch (from->nod_type)
		{
		case nod_agg_count_distinct:
		case nod_agg_total_distinct:
		case nod_agg_average_distinct:
		case nod_agg_average_distinct2:
		case nod_agg_total_distinct2:
		case nod_agg_list_distinct:
			{
				const size_t asbIndex = (from->nod_type == nod_agg_list_distinct) ? 2 : 1;
				const AggregateSort* asb = (AggregateSort*) from->nod_arg[asbIndex];
				impure_agg_sort* asbImpure = (impure_agg_sort*) ((SCHAR*) request + asb->nod_impure);
				SORT_fini(asbImpure->iasb_sort_handle);
				asbImpure->iasb_sort_handle = NULL;
			}
		}
	}
}
