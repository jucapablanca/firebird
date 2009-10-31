/*
 *  The contents of this file are subject to the Initial
 *  Developer's Public License Version 1.0 (the "License");
 *  you may not use this file except in compliance with the
 *  License. You may obtain a copy of the License at
 *  http://www.ibphoenix.com/main.nfs?a=ibphoenix&page=ibp_idpl.
 *
 *  Software distributed under the License is distributed AS IS,
 *  WITHOUT WARRANTY OF ANY KIND, either express or implied.
 *  See the License for the specific language governing rights
 *  and limitations under the License.
 *
 *  The Original Code was created by Dmitry Yemanov
 *  for the Firebird Open Source RDBMS project.
 *
 *  Copyright (c) 2006 Dmitry Yemanov <dimitr@users.sf.net>
 *  and all contributors signed below.
 *
 *  All Rights Reserved.
 *  Contributor(s): ______________________________________.
 *  Adriano dos Santos Fernandes
 */

#ifndef JRD_VIRTUAL_TABLE_H
#define JRD_VIRTUAL_TABLE_H

namespace Jrd {


class RecordStream	//// TODO: create RecordStream.h
{
public:
	RecordStream(RecordSource* aRsb)
		: rsb(aRsb)
	{
	}

	virtual ~RecordStream()
	{
	}

public:
	virtual void findRsbs(StreamStack* streamList, RsbStack* rsbList)
	{
		streamList->push(rsb->rsb_stream);
	}

	virtual void invalidate(thread_db* tdbb, record_param* rpb)
	{
		rpb->rpb_number.setValid(false);
	}

public:
	virtual unsigned dump(UCHAR* buffer, unsigned bufferLen) = 0;
	virtual void open(thread_db* tdbb, jrd_req* request) = 0;
	virtual void close(thread_db* tdbb) = 0;
	virtual bool get(thread_db* tdbb, jrd_req* request) = 0;
	virtual void markRecursive() = 0;

protected:
	RecordSource* rsb;
};


class VirtualTable : public RecordStream
{
private:
	explicit VirtualTable(RecordSource* aRsb)
		: RecordStream(aRsb)
	{
	}

public:
	static RecordSource* create(thread_db*, OptimizerBlk*, SSHORT);
	static void erase(thread_db*, record_param*);
	static void modify(thread_db*, record_param*, record_param*);
	static void store(thread_db*, record_param*);

public:
	virtual unsigned dump(UCHAR* buffer, unsigned bufferLen);
	virtual void open(thread_db* tdbb, jrd_req* request);
	virtual void close(thread_db* tdbb);
	virtual bool get(thread_db* tdbb, jrd_req* request);
	virtual void markRecursive();
};


} // namespace Jrd

#endif // JRD_VIRTUAL_TABLE_H
