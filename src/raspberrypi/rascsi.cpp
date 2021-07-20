//---------------------------------------------------------------------------
//
//	SCSI Target Emulator RaSCSI (*^..^*)
//	for Raspberry Pi
//
//	Powered by XM6 TypeG Technology.
//	Copyright (C) 2016-2020 GIMONS
//	[ RaSCSI main ]
//
//---------------------------------------------------------------------------

#include "os.h"
#include "xm6.h"
#include "filepath.h"
#include "fileio.h"
#include "devices/disk.h"
#include "devices/sasihd.h"
#include "devices/scsihd.h"
#include "devices/scsihd_apple.h"
#include "devices/scsihd_nec.h"
#include "devices/scsicd.h"
#include "devices/scsimo.h"
#include "devices/scsi_host_bridge.h"
#include "devices/scsi_daynaport.h"
#include "controllers/scsidev_ctrl.h"
#include "controllers/sasidev_ctrl.h"
#include "gpiobus.h"
#include "exceptions.h"
#include "rascsi_version.h"
#include "rasutil.h"
#include "rascsi_interface.pb.h"
#include "spdlog/spdlog.h"
#include "spdlog/sinks/stdout_color_sinks.h"
#include <spdlog/async.h>
#include <string>
#include <sstream>
#include <iostream>

using namespace std;
using namespace spdlog;
using namespace rascsi_interface;

//---------------------------------------------------------------------------
//
//  Constant declarations
//
//---------------------------------------------------------------------------
#define CtrlMax	8					// Maximum number of SCSI controllers
#define UnitNum	2					// Number of units around controller
#define FPRT(fp, ...) fprintf(fp, __VA_ARGS__ )

//---------------------------------------------------------------------------
//
//	Variable declarations
//
//---------------------------------------------------------------------------
static volatile BOOL running;		// Running flag
static volatile BOOL active;		// Processing flag
SASIDEV *ctrl[CtrlMax];				// Controller
Disk *disk[CtrlMax * UnitNum];		// Disk
GPIOBUS *bus;						// GPIO Bus
int monsocket;						// Monitor Socket
pthread_t monthread;				// Monitor Thread
pthread_mutex_t ctrl_mutex;					// Semaphore for the ctrl array
static void *MonThread(void *param);

//---------------------------------------------------------------------------
//
//	Signal Processing
//
//---------------------------------------------------------------------------
void KillHandler(int sig)
{
	// Stop instruction
	running = FALSE;
}

//---------------------------------------------------------------------------
//
//	Banner Output
//
//---------------------------------------------------------------------------
void Banner(int argc, char* argv[])
{
	FPRT(stdout,"SCSI Target Emulator RaSCSI(*^..^*) ");
	FPRT(stdout,"version %s (%s, %s)\n",
		rascsi_get_version_string(),
		__DATE__,
		__TIME__);
	FPRT(stdout,"Powered by XM6 TypeG Technology / ");
	FPRT(stdout,"Copyright (C) 2016-2020 GIMONS\n");
	FPRT(stdout,"Connect type : %s\n", CONNECT_DESC);

	if ((argc > 1 && strcmp(argv[1], "-h") == 0) ||
		(argc > 1 && strcmp(argv[1], "--help") == 0)){
		FPRT(stdout,"\n");
		FPRT(stdout,"Usage: %s [-IDn FILE] ...\n\n", argv[0]);
		FPRT(stdout," n is SCSI identification number(0-7).\n");
		FPRT(stdout," FILE is disk image file.\n\n");
		FPRT(stdout,"Usage: %s [-HDn FILE] ...\n\n", argv[0]);
		FPRT(stdout," n is X68000 SASI HD number(0-15).\n");
		FPRT(stdout," FILE is disk image file, \"daynaport\", or \"bridge\".\n\n");
		FPRT(stdout," Image type is detected based on file extension.\n");
		FPRT(stdout,"  hdf : SASI HD image(XM6 SASI HD image)\n");
		FPRT(stdout,"  hds : SCSI HD image(XM6 SCSI HD image)\n");
		FPRT(stdout,"  hdn : SCSI HD image(NEC GENUINE)\n");
		FPRT(stdout,"  hdi : SCSI HD image(Anex86 HD image)\n");
		FPRT(stdout,"  nhd : SCSI HD image(T98Next HD image)\n");
		FPRT(stdout,"  hda : SCSI HD image(APPLE GENUINE)\n");
		FPRT(stdout,"  mos : SCSI MO image(XM6 SCSI MO image)\n");
		FPRT(stdout,"  iso : SCSI CD image(ISO 9660 image)\n");

		exit(0);
	}
}

//---------------------------------------------------------------------------
//
//	Initialization
//
//---------------------------------------------------------------------------
BOOL Init()
{
	struct sockaddr_in server;
	int yes, result;

	result = pthread_mutex_init(&ctrl_mutex,NULL);
	if(result != EXIT_SUCCESS){
		LOGERROR("Unable to create a mutex. Err code: %d", result);
		return FALSE;
	}

	// Create socket for monitor
	monsocket = socket(PF_INET, SOCK_STREAM, 0);
	memset(&server, 0, sizeof(server));
	server.sin_family = PF_INET;
	server.sin_port   = htons(6868);
	server.sin_addr.s_addr = htonl(INADDR_ANY);

	// allow address reuse
	yes = 1;
	if (setsockopt(
		monsocket, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0){
		return FALSE;
	}

	// Bind
	if (bind(monsocket, (struct sockaddr *)&server,
		sizeof(struct sockaddr_in)) < 0) {
		FPRT(stderr, "Error : Already running?\n");
		return FALSE;
	}

	// Create Monitor Thread
	pthread_create(&monthread, NULL, MonThread, NULL);

	// Interrupt handler settings
	if (signal(SIGINT, KillHandler) == SIG_ERR) {
		return FALSE;
	}
	if (signal(SIGHUP, KillHandler) == SIG_ERR) {
		return FALSE;
	}
	if (signal(SIGTERM, KillHandler) == SIG_ERR) {
		return FALSE;
	}

	// GPIOBUS creation
	bus = new GPIOBUS();

	// GPIO Initialization
	if (!bus->Init()) {
		return FALSE;
	}

	// Bus Reset
	bus->Reset();

	// Controller initialization
	for (int i = 0; i < CtrlMax; i++) {
		ctrl[i] = NULL;
	}

	// Disk Initialization
	for (int i = 0; i < CtrlMax; i++) {
		disk[i] = NULL;
	}

	// Other
	running = FALSE;
	active = FALSE;

	return TRUE;
}

//---------------------------------------------------------------------------
//
//	Cleanup
//
//---------------------------------------------------------------------------
void Cleanup()
{
	// Delete the disks
	for (int i = 0; i < CtrlMax * UnitNum; i++) {
		if (disk[i]) {
			delete disk[i];
			disk[i] = NULL;
		}
	}

	// Delete the Controllers
	for (int i = 0; i < CtrlMax; i++) {
		if (ctrl[i]) {
			delete ctrl[i];
			ctrl[i] = NULL;
		}
	}

	// Cleanup the Bus
	bus->Cleanup();

	// Discard the GPIOBUS object
	delete bus;

	// Close the monitor socket
	if (monsocket >= 0) {
		close(monsocket);
	}

	pthread_mutex_destroy(&ctrl_mutex);
}

//---------------------------------------------------------------------------
//
//	Reset
//
//---------------------------------------------------------------------------
void Reset()
{
	// Reset all of the controllers
	for (int i = 0; i < CtrlMax; i++) {
		if (ctrl[i]) {
			ctrl[i]->Reset();
		}
	}

	// Reset the bus
	bus->Reset();
}

//---------------------------------------------------------------------------
//
//	Get the list of attached devices
//
//---------------------------------------------------------------------------
Devices GetDevices() {
	Devices devices;

	for (int i = 0; i < CtrlMax * UnitNum; i++) {
		// skip if unit does not exist or null disk
		Disk *pUnit = disk[i];
		if (!pUnit || pUnit->IsNULL()) {
			continue;
		}

		Device *device = devices.add_devices();

		// Initialize ID and unit number
		device->set_id(i / UnitNum);
		device->set_un(i % UnitNum);

		// ID,UNIT,Type,Device Status
		char type[5];
		type[0] = (char)(pUnit->GetID() >> 24);
		type[1] = (char)(pUnit->GetID() >> 16);
		type[2] = (char)(pUnit->GetID() >> 8);
		type[3] = (char)(pUnit->GetID());
		type[4] = 0;
		device->set_type(type);

		// mount status output
		if (pUnit->GetID() == MAKEID('S', 'C', 'B', 'R')) {
			device->set_file("X68000 HOST BRIDGE");
		} else if (pUnit->GetID() == MAKEID('S', 'C', 'D', 'P')) {
			device->set_file("DaynaPort SCSI/Link");
		} else {
			Filepath filepath;
			pUnit->GetPath(filepath);
			device->set_file(pUnit->IsRemovable() && !pUnit->IsReady() ? "NO MEDIA" : filepath.GetPath());
		}

		// Write protection status
		if (pUnit->IsRemovable() && pUnit->IsReady() && pUnit->IsWriteP()) {
			device->set_read_only(true);
		}
	}

	return devices;
}

//---------------------------------------------------------------------------
//
//	List and log devices
//
//---------------------------------------------------------------------------
string ListDevices(Devices devices) {
	ostringstream s;

	if (devices.devices_size()) {
    	s << endl
    		<< "+----+----+------+-------------------------------------" << endl
    		<< "| ID | UN | TYPE | DEVICE STATUS" << endl
			<< "+----+----+------+-------------------------------------" << endl;

    	LOGINFO( "+----+----+------+-------------------------------------");
    	LOGINFO( "| ID | UN | TYPE | DEVICE STATUS");
    	LOGINFO( "+----+----+------+-------------------------------------\n");
	}
	else {
		return "No images currently attached.\n";
	}

	for (int i = 0; i < devices.devices_size() ; i++) {
		Device device = devices.devices(i);

		s << "|  " << device.id() << " |  " << device.un() << " | " << device.type() << " | "
				<< device.file() << (device.read_only() ? " (WRITEPROTECT)" : "") << endl;

 		LOGINFO( "|  %d |  %d | %s | %s%s", device.id(), device.un(), device.type().c_str(),
 				device.file().c_str(), device.read_only() ? " (WRITEPROTECT)" : "");
	}

	s << "+----+----+------+-------------------------------------" << endl;

    LOGINFO( "+----+----+------+-------------------------------------");

	return s.str();
}

//---------------------------------------------------------------------------
//
//	Controller Mapping
//
//---------------------------------------------------------------------------
bool MapController(Disk **map)
{
	bool status = true;

	// Take ownership of the ctrl data structure
	pthread_mutex_lock(&ctrl_mutex);

	// Replace the changed unit
	for (int i = 0; i < CtrlMax; i++) {
		for (int j = 0; j < UnitNum; j++) {
			int unitno = i * UnitNum + j;
			if (disk[unitno] != map[unitno]) {
				// Check if the original unit exists
				if (disk[unitno]) {
					// Disconnect it from the controller
					if (ctrl[i]) {
						ctrl[i]->SetUnit(j, NULL);
					}

					// Free the Unit
					delete disk[unitno];
				}

				// Setup a new unit
				disk[unitno] = map[unitno];
			}
		}
	}

	// Reconfigure all of the controllers
	for (int i = 0; i < CtrlMax; i++) {
		// Examine the unit configuration
		int sasi_num = 0;
		int scsi_num = 0;
		for (int j = 0; j < UnitNum; j++) {
			int unitno = i * UnitNum + j;
			// branch by unit type
			if (disk[unitno]) {
				if (disk[unitno]->IsSASI()) {
					// Drive is SASI, so increment SASI count
					sasi_num++;
				} else {
					// Drive is SCSI, so increment SCSI count
					scsi_num++;
				}
			}

			// Remove the unit
			if (ctrl[i]) {
				ctrl[i]->SetUnit(j, NULL);
			}
		}

		// If there are no units connected
		if (sasi_num == 0 && scsi_num == 0) {
			if (ctrl[i]) {
				delete ctrl[i];
				ctrl[i] = NULL;
				continue;
			}
		}

		// Mixture of SCSI and SASI
		if (sasi_num > 0 && scsi_num > 0) {
			status = false;
			continue;
		}

		if (sasi_num > 0) {
			// Only SASI Unit(s)

			// Release the controller if it is not SASI
			if (ctrl[i] && !ctrl[i]->IsSASI()) {
				delete ctrl[i];
				ctrl[i] = NULL;
			}

			// Create a new SASI controller
			if (!ctrl[i]) {
				ctrl[i] = new SASIDEV();
				ctrl[i]->Connect(i, bus);
			}
		} else {
			// Only SCSI Unit(s)

			// Release the controller if it is not SCSI
			if (ctrl[i] && !ctrl[i]->IsSCSI()) {
				delete ctrl[i];
				ctrl[i] = NULL;
			}

			// Create a new SCSI controller
			if (!ctrl[i]) {
				ctrl[i] = new SCSIDEV();
				ctrl[i]->Connect(i, bus);
			}
		}

		// connect all units
		for (int j = 0; j < UnitNum; j++) {
			int unitno = i * UnitNum + j;
			if (disk[unitno]) {
				// Add the unit connection
				ctrl[i]->SetUnit(j, disk[unitno]);
			}
		}
	}
	pthread_mutex_unlock(&ctrl_mutex);

	return status;
}

bool ReturnStatus(int fd, bool status = true, const string msg = "") {
	if (fd == -1) {
		if (msg.length()) {
			FPRT(stderr, msg.c_str());
			FPRT(stderr, "\n");
		}
	}
	else {
		Result result;
		result.set_status(status);
		result.set_msg(msg + "\n");

		string data;
		result.SerializeToString(&data);
		SerializeProtobufData(fd, data);
	}

	return status;
}

void SetLogLevel(const string& log_level) {
	if (log_level == "trace") {
		set_level(level::trace);
	}
	else if (log_level == "debug") {
		set_level(level::debug);
	}
	else if (log_level == "info") {
		set_level(level::info);
	}
	else if (log_level == "warn") {
		set_level(level::warn);
	}
	else if (log_level == "err") {
		set_level(level::err);
	}
	else if (log_level == "critical") {
		set_level(level::critical);
	}
	else if (log_level == "off") {
		set_level(level::off);
	}
	else {
		LOGWARN("Invalid log level '%s', falling back to 'trace'", log_level.c_str());
		set_level(level::trace);
	}
}

//---------------------------------------------------------------------------
//
//	Command Processing
//
//---------------------------------------------------------------------------
bool ProcessCmd(int fd, const Command &command)
{
	Disk *map[CtrlMax * UnitNum];
	Filepath filepath;
	Disk *pUnit;
    char type_str[5];

    int id = command.id();
	int un = command.un();
	Operation cmd = command.cmd();
	DeviceType type = command.type();
	string params = command.params().c_str();

	ostringstream s;
	s << "Processing: cmd=" << cmd << ", id=" << id << ", un=" << un << ", type=" << type << ", params=" << params << endl;
	LOGINFO("%s", s.str().c_str());

	// Copy the Unit List
	memcpy(map, disk, sizeof(disk));

	// Check the Controller Number
	if (id < 0 || id >= CtrlMax) {
		return ReturnStatus(fd, false, "Error : Invalid ID");
	}

	// Check the Unit Number
	if (un < 0 || un >= UnitNum) {
		return ReturnStatus(fd, false, "Error : Invalid unit number");
	}

	// Connect Command
	if (cmd == ATTACH) {
		string ext;

		// Distinguish between SASI and SCSI
		if (type == SASI_HD) {
			// Check the extension
			int len = params.length();
			if (len < 5 || params[len - 4] != '.') {
				return ReturnStatus(fd, false);
			}

			// If the extension is not SASI type, replace with SCSI
			ext = params.substr(len - 3);
			if (ext != "hdf") {
				type = SCSI_HD;
			}
		}

		// Create a new drive, based upon type
		pUnit = NULL;
		switch (type) {
			case SASI_HD:		// HDF
				pUnit = new SASIHD();
				break;
			case SCSI_HD:		// HDS/HDN/HDI/NHD/HDA
				if (ext == "hdn" || ext == "hdi" || ext == "nhd") {
					pUnit = new SCSIHD_NEC();
				} else if (ext == "hda") {
					pUnit = new SCSIHD_APPLE();
				} else {
					pUnit = new SCSIHD();
				}
				break;
			case MO:
				pUnit = new SCSIMO();
				break;
			case CD:
				pUnit = new SCSICD();
				break;
			case BR:
				pUnit = new SCSIBR();
				break;
			// case NUVOLINK:
			// 	pUnit = new SCSINuvolink();
			// 	break;
			case DAYNAPORT:
				pUnit = new SCSIDaynaPort();
				break;
			default:
				ostringstream error;
				error << "rasctl sent a command for an invalid drive type: " << type;
				return ReturnStatus(fd, false, error.str());
		}

		// drive checks files
		if (type != BR && type != DAYNAPORT && !command.params().empty()) {
			// Strip the image file extension from device file names, so that device files can be used as drive images
			string file = params.find("/dev/") ? params : params.substr(0, params.length() - 4);

			// Set the Path
			filepath.SetPath(file.c_str());

			// Open the file path
			if (!pUnit->Open(filepath)) {
				delete pUnit;

				LOGWARN("rasctl tried to open an invalid file %s", file.c_str());

				ostringstream error;
				error << "File open error [" << file << "]";
				return ReturnStatus(fd, false, error.str());
			}
		}

		// Set the cache to write-through
		pUnit->SetCacheWB(FALSE);

		// Replace with the newly created unit
		map[id * UnitNum + un] = pUnit;

		// Re-map the controller
		bool status = MapController(map);
		if (status) {
			type_str[0] = (char)(pUnit->GetID() >> 24);
	        type_str[1] = (char)(pUnit->GetID() >> 16);
	        type_str[2] = (char)(pUnit->GetID() >> 8);
	        type_str[3] = (char)(pUnit->GetID());
	        type_str[4] = '\0';

	        LOGINFO("rasctl added new %s device. ID: %d UN: %d", type_str, id, un);
		}

		return ReturnStatus(fd, status, status ? "" : "Error : SASI and SCSI can't be mixed\n");
	}

	// Does the controller exist?
	if (ctrl[id] == NULL) {
		LOGWARN("rasctl sent a command for invalid controller %d", id);

		return ReturnStatus(fd, false, "Error : No such device");
	}

	// Does the unit exist?
	pUnit = disk[id * UnitNum + un];
	if (pUnit == NULL) {
		LOGWARN("rasctl sent a command for invalid unit ID %d UN %d", id, un);

		return ReturnStatus(fd, false, "Error : No such device");
	}

	type_str[0] = (char)(pUnit->GetID() >> 24);
    type_str[1] = (char)(pUnit->GetID() >> 16);
    type_str[2] = (char)(pUnit->GetID() >> 8);
    type_str[3] = (char)(pUnit->GetID());
    type_str[4] = '\0';

	// Disconnect Command
	if (cmd == DETACH) {
		LOGINFO("rasctl command disconnect %s at ID: %d UN: %d", type_str, id, un);

		// Free the existing unit
		map[id * UnitNum + un] = NULL;

		// Re-map the controller
		bool status = MapController(map);
		return ReturnStatus(fd, status, status ? "" : "Error : SASI and SCSI can't be mixed\n");
	}

	// Valid only for MO or CD
	if (pUnit->GetID() != MAKEID('S', 'C', 'M', 'O') &&
		pUnit->GetID() != MAKEID('S', 'C', 'C', 'D')) {
		LOGWARN("rasctl sent an Insert/Eject/Protect command (%d) for incompatible type %s", cmd, type_str);

		ostringstream error;
		error << "Operation denied (Device type " << type_str << " isn't removable)";
		return ReturnStatus(fd, false, error.str());
	}

	switch (cmd) {
		case INSERT:
			filepath.SetPath(params.c_str());
			LOGINFO("rasctl commanded insert file %s into %s ID: %d UN: %d", params.c_str(), type_str, id, un);

			if (!pUnit->Open(filepath)) {
				ostringstream error;
				error << "File open error [" << params << "]";

				return ReturnStatus(fd, false, error.str());
			}
			break;

		case EJECT:
			LOGINFO("rasctl commands eject %s ID: %d UN: %d", type_str, id, un);
			pUnit->Eject(TRUE);
			break;

		case PROTECT:
			if (pUnit->GetID() != MAKEID('S', 'C', 'M', 'O')) {
				LOGWARN("rasctl sent an invalid PROTECT command for %s ID: %d UN: %d", type_str, id, un);

				return ReturnStatus(fd, false, "Error : Operation denied (Device isn't MO)");
			}
			LOGINFO("rasctl is setting write protect to %d for %s ID: %d UN: %d",!pUnit->IsWriteP(), type_str, id, un);
			pUnit->WriteP(!pUnit->IsWriteP());
			break;

		default:
			ostringstream error;
			error << "Received unknown command from rasctl: " << cmd;
			LOGWARN("%s", error.str().c_str());
			return ReturnStatus(fd, false, error.str());
	}

	return ReturnStatus(fd, true);
}

bool has_suffix(const string& filename, const string& suffix) {
    return filename.size() >= suffix.size() && !filename.compare(filename.size() - suffix.size(), suffix.size(), suffix);
}

//---------------------------------------------------------------------------
//
//	Argument Parsing
//
//---------------------------------------------------------------------------
bool ParseArgument(int argc, char* argv[])
{
	int id = -1;
	bool is_sasi = false;
	int max_id = 7;
	string log_level = "trace";

	int opt;
	while ((opt = getopt(argc, argv, "-IiHhG:g:D:d:")) != -1) {
		switch (tolower(opt)) {
			case 'i':
				is_sasi = false;
				max_id = 7;
				id = -1;
				continue;

			case 'h':
				is_sasi = true;
				max_id = 15;
				id = -1;
				continue;

			case 'g':
				log_level = optarg;
				continue;

			case 'd': {
				char* end;
				id = strtol(optarg, &end, 10);
				if (*end || id < 0 || max_id < id) {
					cerr << optarg << ": invalid " << (is_sasi ? "HD" : "ID") << " (0-" << max_id << ")" << endl;
					return false;
				}
				continue;
			}

			default:
				return false;

			case 1:
				break;
		}

		if (id < 0) {
			cerr << optarg << ": ID not specified" << endl;
			return false;
		} else if (disk[id] && !disk[id]->IsNULL()) {
			cerr << id << ": duplicate ID" << endl;
			return false;
		}

		string path = optarg;
		DeviceType type = SASI_HD;
		if (has_suffix(path, ".hdf") || has_suffix(path, ".hds") || has_suffix(path, ".hdn")
			|| has_suffix(path, ".hdi") || has_suffix(path, ".hda") || has_suffix(path, ".nhd")) {
			type = SASI_HD;
		} else if (has_suffix(path, ".mos")) {
			type = MO;
		} else if (has_suffix(path, ".iso")) {
			type = CD;
		} else if (path == "bridge") {
			type = BR;
		} else if (path == "daynaport") {
			type = DAYNAPORT;
		} else {
			cerr << path << ": unknown file extension or basename is missing" << endl;
		    return false;
		}

		int un = 0;
		if (is_sasi) {
			un = id % UnitNum;
			id /= UnitNum;
		}

		// Execute the command
		Command command;
		command.set_id(id);
		command.set_un(un);
		command.set_cmd(ATTACH);
		command.set_type(type);
		command.set_params(path);
		if (!ProcessCmd(-1, command)) {
			return false;
		}
		id = -1;
	}

	SetLogLevel(log_level);

	// Display the device list
	Devices devices = GetDevices();
	cout << ListDevices(devices) << endl;

	return true;
}

//---------------------------------------------------------------------------
//
//	Pin the thread to a specific CPU
//
//---------------------------------------------------------------------------
void FixCpu(int cpu)
{
	// Get the number of CPUs
	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	sched_getaffinity(0, sizeof(cpu_set_t), &cpuset);
	int cpus = CPU_COUNT(&cpuset);

	// Set the thread affinity
	if (cpu < cpus) {
		CPU_ZERO(&cpuset);
		CPU_SET(cpu, &cpuset);
		sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
	}
}

//---------------------------------------------------------------------------
//
//	Monitor Thread
//
//---------------------------------------------------------------------------
static void *MonThread(void *param)
{
	int fd;

    // Scheduler Settings
	struct sched_param schedparam;
	schedparam.sched_priority = 0;
	sched_setscheduler(0, SCHED_IDLE, &schedparam);

	// Set the affinity to a specific processor core
	FixCpu(2);

	// Wait for the execution to start
	while (!running) {
		usleep(1);
	}

	// Set up the monitor socket to receive commands
	listen(monsocket, 1);

	try {
		while (true) {
			// Wait for connection
			struct sockaddr_in client;
			socklen_t socklen = sizeof(client);
			memset(&client, 0, socklen);
			fd = accept(monsocket, (struct sockaddr*)&client, &socklen);
			if (fd < 0) {
				throw ioexception("accept() failed");
			}

			// Fetch the command
			Command command;
			command.ParseFromString(DeserializeProtobufData(fd));

			// List all of the devices
			if (command.cmd() == LIST) {
				Devices devices = GetDevices();
				ListDevices(devices);

				string data;
				devices.SerializeToString(&data);
				SerializeProtobufData(fd, data);
			}
			else if (command.cmd() == LOG_LEVEL) {
				SetLogLevel(command.params());
			}
			else {
				// Wait until we become idle
				while (active) {
					usleep(500 * 1000);
				}

				ProcessCmd(fd, command);
			}

			close(fd);
			fd = -1;
		}
	}
	catch(const ioexception& e) {
		LOGERROR("%s", e.getmsg());

		// Fall through
	}

    if (fd >= 0) {
		close(fd);
	}

	return NULL;
}

//---------------------------------------------------------------------------
//
//	Main processing
//
//---------------------------------------------------------------------------
int main(int argc, char* argv[])
{
	GOOGLE_PROTOBUF_VERIFY_VERSION;

	int i;
	int actid;
	DWORD now;
	BUS::phase_t phase;
	BYTE data;
	// added setvbuf to override stdout buffering, so logs are written immediately and not when the process exits.
	setvbuf(stdout, NULL, _IONBF, 0);
	struct sched_param schparam;

	set_level(level::trace);
	// Create a thread-safe stdout logger to process the log messages
	auto logger = stdout_color_mt("rascsi stdout logger");

	// Output the Banner
	Banner(argc, argv);

	// Initialize
	int ret = 0;
	if (!Init()) {
		ret = EPERM;
		goto init_exit;
	}

	// Reset
	Reset();

	// Argument parsing
	if (!ParseArgument(argc, argv)) {
		ret = EINVAL;
		goto err_exit;
	}

    // Set the affinity to a specific processor core
	FixCpu(3);

#ifdef USE_SEL_EVENT_ENABLE
	// Scheduling policy setting (highest priority)
	schparam.sched_priority = sched_get_priority_max(SCHED_FIFO);
	sched_setscheduler(0, SCHED_FIFO, &schparam);
#endif	// USE_SEL_EVENT_ENABLE

	// Start execution
	running = TRUE;

	// Main Loop
	while (running) {
		// Work initialization
		actid = -1;
		phase = BUS::busfree;

#ifdef USE_SEL_EVENT_ENABLE
		// SEL signal polling
		if (bus->PollSelectEvent() < 0) {
			// Stop on interrupt
			if (errno == EINTR) {
				break;
			}
			continue;
		}

		// Get the bus
		bus->Aquire();
#else
		bus->Aquire();
		if (!bus->GetSEL()) {
			usleep(0);
			continue;
		}
#endif	// USE_SEL_EVENT_ENABLE

        // Wait until BSY is released as there is a possibility for the
        // initiator to assert it while setting the ID (for up to 3 seconds)
		if (bus->GetBSY()) {
			now = SysTimer::GetTimerLow();
			while ((SysTimer::GetTimerLow() - now) < 3 * 1000 * 1000) {
				bus->Aquire();
				if (!bus->GetBSY()) {
					break;
				}
			}
		}

		// Stop because it the bus is busy or another device responded
		if (bus->GetBSY() || !bus->GetSEL()) {
			continue;
		}

		pthread_mutex_lock(&ctrl_mutex);

		// Notify all controllers
		data = bus->GetDAT();
		for (i = 0; i < CtrlMax; i++) {
			if (!ctrl[i] || (data & (1 << i)) == 0) {
				continue;
			}

			// Find the target that has moved to the selection phase
			if (ctrl[i]->Process() == BUS::selection) {
				// Get the target ID
				actid = i;

				// Bus Selection phase
				phase = BUS::selection;
				break;
			}
		}

		// Return to bus monitoring if the selection phase has not started
		if (phase != BUS::selection) {
			pthread_mutex_unlock(&ctrl_mutex);
			continue;
		}

		// Start target device
		active = TRUE;

#ifndef USE_SEL_EVENT_ENABLE
		// Scheduling policy setting (highest priority)
		schparam.sched_priority = sched_get_priority_max(SCHED_FIFO);
		sched_setscheduler(0, SCHED_FIFO, &schparam);
#endif	// USE_SEL_EVENT_ENABLE

		// Loop until the bus is free
		while (running) {
			// Target drive
			phase = ctrl[actid]->Process();

			// End when the bus is free
			if (phase == BUS::busfree) {
				break;
			}
		}
		pthread_mutex_unlock(&ctrl_mutex);


#ifndef USE_SEL_EVENT_ENABLE
		// Set the scheduling priority back to normal
		schparam.sched_priority = 0;
		sched_setscheduler(0, SCHED_OTHER, &schparam);
#endif	// USE_SEL_EVENT_ENABLE

		// End the target travel
		active = FALSE;
	}

err_exit:
	// Cleanup
	Cleanup();

init_exit:
	return ret;
}
