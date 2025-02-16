#include "stdafx.h"
#include "Honeywell.h"
#include "../main/Helper.h"
#include "../main/Logger.h"
#include "hardwaretypes.h"
#include "../main/WebServerHelper.h"
#include "../main/RFXtrx.h"
#include "../main/SQLHelper.h"
#include "../httpclient/HTTPClient.h"
#include "../main/mainworker.h"
#include "../main/json_helper.h"
#include "../webserver/Base64.h"

constexpr auto HONEYWELL_DEFAULT_APIKEY = "atD3jtzXC5z4X8WPbzvo0CBqWi7S81Nh";
constexpr auto HONEYWELL_DEFAULT_APISECRET = "TXDzy2aHpAJw6YiO";
constexpr auto HONEYWELL_LOCATIONS_PATH = "https://api.honeywell.com/v2/locations?apikey=[apikey]";
constexpr auto HONEYWELL_UPDATE_THERMOSTAT = "https://api.honeywell.com/v2/devices/thermostats/[deviceid]?apikey=[apikey]&locationId=[locationid]";
constexpr auto HONEYWELL_TOKEN_PATH = "https://api.honeywell.com/oauth2/token";

constexpr auto kSetPointDesc = "Target temperature ([devicename])";
constexpr auto kHeatingDesc = "Heating Mode([devicename])";
constexpr auto kCoolingDesc = "Cooling Mome([devicename])";
constexpr auto kHeatingStatusDesc = "Heating State ([devicename])";
constexpr auto kCoolingStatusDesc = "Cooling State ([devicename])";
constexpr auto kOutdoorTempDesc = "Outdoor temperature ([devicename])";
constexpr auto kRoomTempDesc = "Room temperature ([devicename])";
constexpr auto kAwayDesc = "Away ([name])";
constexpr auto kfanRequest = "Fan Request ([devicename])";
constexpr auto kcirculationFanRequest = "Circulation Fan Request ([devicename])";

extern http::server::CWebServerHelper m_webservers;

CHoneywell::CHoneywell(const int ID, const std::string &Username, const std::string &Password, const std::string &Extra)
{
	m_HwdID = ID;
	mAccessToken = Username;
	mRefreshToken = Password;
	stdstring_trim(mAccessToken);
	stdstring_trim(mRefreshToken);

        // get the data from the extradata field
        std::vector<std::string> strextra;
        StringSplit(Extra, "|", strextra);
	if (strextra.size() == 2)
	{
		mApiKey = base64_decode(strextra[0]);
		mApiSecret = base64_decode(strextra[1]);
	}
	if (mApiKey.empty()) {
		Log(LOG_STATUS, "No API key was set. Using default API key. This will result in many errors caused by quota limitations.");
		mApiKey = HONEYWELL_DEFAULT_APIKEY;
		mApiSecret = HONEYWELL_DEFAULT_APISECRET;
	}
	if (Username.empty() || Password.empty()) {
		Log(LOG_ERROR, "Please update your access token/request token!...");
	}
	mLastMinute = -1;
	Init();
}

void CHoneywell::Init()
{
	mTokenExpires = mytime(nullptr);
}

bool CHoneywell::StartHardware()
{
	RequestStart();

	Init();
	mLastMinute = -1;
	//Start worker thread
	m_thread = std::make_shared<std::thread>([this] { Do_Work(); });
	SetThreadNameInt(m_thread->native_handle());
	mIsStarted = true;
	sOnConnected(this);
	return (m_thread != nullptr);
}

bool CHoneywell::StopHardware()
{
	if (m_thread)
	{
		RequestStop();
		m_thread->join();
		m_thread.reset();
	}

	mIsStarted = false;
	return true;
}

#define HONEYWELL_POLL_INTERVAL 300 // 5 minutes
#define HWAPITIMEOUT 30 // 30 seconds

//
// worker thread
//
void CHoneywell::Do_Work()
{
	Log(LOG_STATUS, "Worker started...");
	int sec_counter = HONEYWELL_POLL_INTERVAL - 5;
	while (!IsStopRequested(1000))
	{
		sec_counter++;
		if (sec_counter % 12 == 0) {
			m_LastHeartbeat = mytime(nullptr);
		}
		if (sec_counter % HONEYWELL_POLL_INTERVAL == 0)
		{
			GetThermostatData();
		}
	}
	Log(LOG_STATUS, "Worker stopped...");
}


//
// callback from Domoticz backend to update the Honeywell thermostat
//
bool CHoneywell::WriteToHardware(const char *pdata, const unsigned char /*length*/)
{
	
	const tRBUF *pCmd = reinterpret_cast<const tRBUF *>(pdata);
	
	if (pCmd->LIGHTING2.packettype == pTypeLighting2)
	{
		//Light command
		
		int nodeID = pCmd->LIGHTING2.id4;
		int devID = nodeID / 10;
		std::string deviceName = mDeviceList[devID]["name"].asString();
		bool bIsOn = (pCmd->LIGHTING2.cmnd == light2_sOn);
		if ((nodeID % 10) == 3 || (nodeID % 10) == 7) {
			// heating cooling on or off
			SetPauseStatus(devID, bIsOn, nodeID);
			return true;
		}

	}
	else if (pCmd->ICMND.packettype == pTypeSetpoint && pCmd->LIGHTING2.subtype == sTypeSetpoint)
	{
		int nodeID = pCmd->LIGHTING2.id3;
		int devID = nodeID / 10;
		const _tSetpoint* therm = reinterpret_cast<const _tSetpoint*>(pdata);
		SetSetpoint(devID, therm->value, nodeID);
	}
	return false;
}

//
// refresh the OAuth2 token through Honeywell API
//
bool CHoneywell::refreshToken()
{
	if (mRefreshToken.empty())
		return false;

	if (mTokenExpires > mytime(nullptr))
		return true;

	std::string sResult;

	std::string postData = "grant_type=refresh_token&refresh_token=[refreshToken]";
	stdreplace(postData, "[refreshToken]", mRefreshToken);

	std::string auth = mApiKey;
	auth += ":";
	auth += mApiSecret;
	std::string encodedAuth = base64_encode(auth);


	std::vector<std::string> headers;
	std::string authHeader = "Authorization: [auth]";
	stdreplace(authHeader, "[auth]", encodedAuth);
	headers.push_back(authHeader);
	headers.push_back("Content-Type: application/x-www-form-urlencoded");

	HTTPClient::SetConnectionTimeout(HWAPITIMEOUT);
	HTTPClient::SetTimeout(HWAPITIMEOUT);
	if (!HTTPClient::POST(HONEYWELL_TOKEN_PATH, postData, headers, sResult)) {
		Log(LOG_ERROR, "Error refreshing token");
		return false;
	}

	Json::Value root;
	bool ret = ParseJSon(sResult, root);
	if (!ret) {
		Log(LOG_ERROR, "Invalid/no data received...");
		return false;
	}

	std::string at = root["access_token"].asString();
	std::string rt = root["refresh_token"].asString();
	std::string ei = root["expires_in"].asString();
	if (at.length() && rt.length() && ei.length()) {
		int expires_in = std::stoi(ei);
		mTokenExpires = mytime(nullptr) + (expires_in > 0 ? expires_in : 600) - HWAPITIMEOUT;
		mAccessToken = at;
		mRefreshToken = rt;
		Log(LOG_NORM, "Storing received access & refresh token. Token expires after %d seconds.",expires_in);
		m_sql.safe_query("UPDATE Hardware SET Username='%q', Password='%q' WHERE (ID==%d)", mAccessToken.c_str(), mRefreshToken.c_str(), m_HwdID);
		mSessionHeaders.clear();
		mSessionHeaders.push_back("Authorization:Bearer " + mAccessToken);
		mSessionHeaders.push_back("Content-Type: application/json");
	}
	else
		return false;

	return true;
}

//
// Get honeywell data through Honeywell API
//
void CHoneywell::GetThermostatData()
{
	if (!refreshToken())
		return;

	std::string sResult;
	std::string sURL = HONEYWELL_LOCATIONS_PATH;
	stdreplace(sURL, "[apikey]", mApiKey);
	
	HTTPClient::SetConnectionTimeout(HWAPITIMEOUT);
	HTTPClient::SetTimeout(HWAPITIMEOUT);
	if (!HTTPClient::GET(sURL, mSessionHeaders, sResult)) {
		Log(LOG_ERROR, "Error getting thermostat data!");
		return;
	}

	Json::Value root;
	bool ret = ParseJSon(sResult, root);
	if (!ret) {
		Log(LOG_ERROR, "Invalid/no data received...");
		return;
	}

	int devNr = 0;
	mDeviceList.clear();
	mLocationList.clear();
	for (auto location : root)
	{
		Json::Value devices = location["devices"];
		for (auto device : devices)
		{

			std::string deviceName = device["name"].asString();
			mDeviceList[devNr] = device;
			mLocationList[devNr] = location["locationID"].asString();

			std::string units = device["units"].asString(); //:"Fahrenheit"

			float temperature;
			temperature = (float)device["indoorTemperature"].asFloat();
			std::string desc = kRoomTempDesc;
			stdreplace(desc, "[devicename]", deviceName);
			if(units == "Fahrenheit") {
				SendTempSensor(10 * devNr + 1, 255, (float)ConvertToCelsius(temperature), desc);
			}else{
				SendTempSensor(10 * devNr + 1, 255, temperature, desc);
			}

			temperature = (float)device["outdoorTemperature"].asFloat();
			desc = kOutdoorTempDesc;
			stdreplace(desc, "[devicename]", deviceName);
			if(units == "Fahrenheit") {
				SendTempSensor(10 * devNr + 2, 255, (float)ConvertToCelsius(temperature), desc);
			}else{
				SendTempSensor(10 * devNr + 2, 255, temperature, desc);
			}

			std::string mode = device["changeableValues"]["mode"].asString();
			bool bHeating = (mode == "Heat");
			desc = kHeatingDesc;
			stdreplace(desc, "[devicename]", deviceName);
			SendSwitch(10 * devNr + 3, 1, 255, bHeating, 0, desc, m_Name);

			if(bHeating){
				temperature = (float)device["changeableValues"]["heatSetpoint"].asFloat();
			}else{
				temperature = (float)device["changeableValues"]["coolSetpoint"].asFloat();
			}
			desc = kSetPointDesc;
			stdreplace(desc, "[devicename]", deviceName);
			if(units == "Fahrenheit") {
				SendSetPointSensor(0, 0, 0, (uint8_t)(10 * devNr + 4), 0, 255, (float)ConvertToCelsius(temperature), desc);
			}else{
				SendSetPointSensor(0, 0, 0, (uint8_t)(10 * devNr + 4), 0, 255, temperature, desc);
			}
			
			std::string operationstatus = device["operationStatus"]["mode"].asString();
			bool bStatus = (operationstatus == "Heat");
			desc = kHeatingStatusDesc;
			stdreplace(desc, "[devicename]", deviceName);
			SendSwitch(10 * devNr + 5, 1, 255, bStatus, 0, desc, m_Name);

			//std::string operationstatus = device["operationStatus"]["mode"].asString();
			bool bCStatus = (operationstatus == "Cool");
			desc = kCoolingStatusDesc;
			stdreplace(desc, "[devicename]", deviceName);
			SendSwitch(10 * devNr + 6, 1, 255, bCStatus, 0, desc, m_Name);

			//std::string mode = device["changeableValues"]["mode"].asString();
			bool bCooling = (mode == "Cool");
			desc = kCoolingDesc;
			stdreplace(desc, "[devicename]", deviceName);
			SendSwitch(10 * devNr + 7, 1, 255, bCooling, 0, desc, m_Name);

			bool fanRequest = device["operationStatus"]["fanRequest"].asBool();
			desc = kfanRequest;
			stdreplace(desc, "[devicename]", deviceName);
			SendSwitch(10 * devNr + 8, 1, 255, fanRequest, 0, desc, m_Name);

			bool circulationFanRequest = device["operationStatus"]["circulationFanRequest"].asBool();
			desc = kcirculationFanRequest;
			stdreplace(desc, "[devicename]", deviceName);
			SendSwitch(10 * devNr + 9, 1, 255, circulationFanRequest, 0, desc, m_Name);

			devNr++;
		}

		bool geoFenceEnabled = location["geoFenceEnabled"].asBool();
		if (geoFenceEnabled) {
			
			Json::Value geofences = location["geoFences"];
			bool bAway = true;
			for (auto &geofence : geofences)
			{
				int withinFence = geofence["geoOccupancy"]["withinFence"].asInt();
				if (withinFence > 0) {
					bAway = false;
					break;
				}
			}
			std::string desc = kAwayDesc;
			stdreplace(desc, "[name]", location["name"].asString());
			SendSwitch(10 * devNr + 6, 1, 255, bAway, 0, desc, m_Name);
		}
	}
}

//
// transfer pause status to Honeywell api
//
void CHoneywell::SetPauseStatus(const int idx, bool bCommand, const int nodeID)
{
	if (!refreshToken()) {
		Log(LOG_ERROR,"No token available. Failed setting thermostat data");
		return;
	}

	std::string currentMode = mDeviceList[idx]["changeableValues"]["mode"].asString();

	bool bHeating = currentMode == "Heat";
	bool nHeat = currentMode == "Heat";
	bool nCool = currentMode == "Cool";
	
	if ((nodeID % 10) == 3){
		nHeat = bCommand;
		bHeating = true;
		if(bCommand){
			nCool = false;
		}
	}
	if ((nodeID % 10) == 7){
		nCool = bCommand;
		bHeating = false;
		if(bCommand){
			nHeat = false;
		}
	}
	
	
	std::string url = HONEYWELL_UPDATE_THERMOSTAT;
	std::string deviceID = mDeviceList[idx]["deviceID"].asString();

	stdreplace(url, "[deviceid]", deviceID);
	stdreplace(url, "[apikey]", mApiKey);
	stdreplace(url, "[locationid]", mLocationList[idx]);

	Json::Value reqRoot;
	reqRoot["mode"] =         nHeat ? "Heat" : (nCool ? "Cool" : "Off");
	reqRoot["autoChangeoverActive"] = "true";
	reqRoot["heatSetpoint"] = mDeviceList[idx]["changeableValues"]["heatSetpoint"].asInt();
	reqRoot["coolSetpoint"] = mDeviceList[idx]["changeableValues"]["coolSetpoint"].asInt();
	reqRoot["thermostatSetpointStatus"] = "TemporaryHold";

	std::string sResult;
	HTTPClient::SetConnectionTimeout(HWAPITIMEOUT);
	HTTPClient::SetTimeout(HWAPITIMEOUT);
	if (!HTTPClient::POST(url, JSonToRawString(reqRoot), mSessionHeaders, sResult, true, true)) {
		Log(LOG_ERROR, "Error setting thermostat data!");
		return;
	}
	std::string desc = kHeatingDesc;
	stdreplace(desc, "[devicename]", mDeviceList[idx]["name"].asString());
	SendSwitch(10 * idx + 3, 1, 255, nHeat, 0, desc, m_Name);
	
	desc = kCoolingDesc;
	stdreplace(desc, "[devicename]", mDeviceList[idx]["name"].asString());
	SendSwitch(10 * idx + 7, 1, 255, nCool, 0, desc, m_Name);
	
	if(bCommand){
		std::string units = mDeviceList[idx]["units"].asString();
		float temperature;
		if(nHeat){
			temperature = (float)mDeviceList[idx]["changeableValues"]["heatSetpoint"].asFloat();
		}else{
			temperature = (float)mDeviceList[idx]["changeableValues"]["coolSetpoint"].asFloat();
		}
		desc = kSetPointDesc;
		stdreplace(desc, "[devicename]", mDeviceList[idx]["name"].asString());
		if(units == "Fahrenheit") {
			SendSetPointSensor(0, 0, 0, (uint8_t)(10 * idx + 4), 0, 255, (float)ConvertToCelsius(temperature), desc);
		}else{
			SendSetPointSensor(0, 0, 0, (uint8_t)(10 * idx + 4), 0, 255, temperature, desc);
		}
	}

	
}

//
// transfer the updated temperature to Honeywell API
//
void CHoneywell::SetSetpoint(const int idx, const float temp, const int nodeid)
{
	if (!refreshToken()) {
		Log(LOG_ERROR, "No token available. Error setting thermostat data!");
		return;
	}

	std::string url = HONEYWELL_UPDATE_THERMOSTAT;
	std::string deviceID = mDeviceList[idx]["deviceID"].asString();

	stdreplace(url, "[deviceid]", deviceID);
	stdreplace(url, "[apikey]", mApiKey);
	stdreplace(url, "[locationid]", mLocationList[idx]);

	Json::Value reqRoot;
	std::string units = mDeviceList[idx]["units"].asString();
	
	if(GetSwitchValue((uint8_t)(10 * idx + 7)) /* mode set for cooling*/){
		reqRoot["mode"] = "Cool";
		reqRoot["autoChangeoverActive"] = "true";
		reqRoot["heatSetpoint"] = mDeviceList[idx]["changeableValues"]["heatSetpoint"].asInt();
	
		if(units == "Fahrenheit") {
			reqRoot["coolSetpoint"] = (float)ConvertToFahrenheit(temp);
		}else{
			reqRoot["coolSetpoint"] = temp;
		}
	}else{
		reqRoot["mode"] = "Heat";
		reqRoot["autoChangeoverActive"] = "true";
		if(units == "Fahrenheit") {
			reqRoot["heatSetpoint"] = (float)ConvertToFahrenheit(temp);
		}else{
			reqRoot["heatSetpoint"] = temp;
		}
		reqRoot["coolSetpoint"] = mDeviceList[idx]["changeableValues"]["coolSetpoint"].asInt();
	}
		
	//reqRoot["thermostatSetpointStatus"] = "PermanentHold";
	reqRoot["thermostatSetpointStatus"] = "TemporaryHold";
	std::string sResult;
	if(GetSwitchValue((uint8_t)(10 * idx + 7)) | GetSwitchValue((uint8_t)(10 * idx + 3))){
		HTTPClient::SetConnectionTimeout(HWAPITIMEOUT);
		HTTPClient::SetTimeout(HWAPITIMEOUT);
		if (!HTTPClient::POST(url, JSonToRawString(reqRoot), mSessionHeaders, sResult, true, true)) {
			Log(LOG_ERROR, "Error setting thermostat data!");
			return;
		}

		std::string desc = kSetPointDesc;
		stdreplace(desc, "[devicename]", mDeviceList[idx]["name"].asString());
		SendSetPointSensor(0, 0, 0, (uint8_t)(10 * idx + 4), 0, 255, temp, desc);
	}
	//desc = kHeatingDesc;
	//stdreplace(desc, "[devicename]", mDeviceList[idx]["name"].asString());
	//SendSwitch(10 * idx + 3, 1, 255, true, 0, desc);
}

bool CHoneywell::GetSwitchValue(const int NodeID)
{
	uint8_t ChildID = 1;
	//make device ID
	unsigned char ID1 = (unsigned char)((NodeID & 0xFF000000) >> 24);
	unsigned char ID2 = (unsigned char)((NodeID & 0xFF0000) >> 16);
	unsigned char ID3 = (unsigned char)((NodeID & 0xFF00) >> 8);
	unsigned char ID4 = (unsigned char)NodeID & 0xFF;

	char szIdx[10];
	sprintf(szIdx, "%X%02X%02X%02X", ID1, ID2, ID3, ID4);
	std::vector<std::vector<std::string> > result;
	result = m_sql.safe_query("SELECT Name,nValue,sValue FROM DeviceStatus WHERE (HardwareID==%d) AND (DeviceID=='%q') AND (Unit == %d) AND (Type==%d) AND (Subtype==%d)",
		m_HwdID, szIdx, ChildID, int(pTypeLighting2), int(sTypeAC));
	if (!result.empty())
	{
		int nvalue = atoi(result[0][1].c_str());
		return (nvalue != light2_sOff);
	}

	Log(LOG_ERROR, "Error reading switch data!");
	return false;
}
