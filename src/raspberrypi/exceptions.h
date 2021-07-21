//---------------------------------------------------------------------------
//
//      SCSI Target Emulator RaSCSI (*^..^*)
//      for Raspberry Pi
//
//      Powered by XM6 TypeG Technology.
//      Copyright (C) 2016-2020 GIMONS
//      [ Exceptions ]
//
//---------------------------------------------------------------------------

#if !defined(exceptions_h)
#define exceptions_h

#include <exception>
#include <string>

using namespace std;

class lunexception final : public exception {
private:
        int lun;

public:
        lunexception(int _lun) : lun(_lun) { }

        ~lunexception() { }

        int getlun() const {
            return lun;
        }
};

class ioexception : public exception {
private:
	string msg;

public:
	ioexception(const string& _msg) : msg(_msg) { }

	~ioexception() { }

	const string& getmsg() const {
		return msg;
	}
};

#endif
