#pragma once

#include <deque>
#include <iostream>
#include "ASyncTCP.h"
#include "DomoticzHardware.h"

class Comm5SMTCP : public CDomoticzHardwareBase, ASyncTCP
{
public:
	Comm5SMTCP(const int ID, const std::string &IPAddress, const unsigned short usIPPort);
	bool WriteToHardware(const char *pdata, const unsigned char length) override;
	boost::signals2::signal<void()>	sDisconnected;
private:
	bool StartHardware() override;
	bool StopHardware() override;
protected:
	void OnConnect();
	void OnDisconnect();
	void OnData(const unsigned char *pData, size_t length);
	void OnErrorStd(const std::exception e);
	void OnErrorBoost(const boost::system::error_code& error);
	void Do_Work();
	void ParseData(const unsigned char *data, const size_t len);
	void querySensorState();
private:
	std::string m_szIPAddress;
	unsigned short m_usIPPort;
	std::string buffer;
	bool initSensorData;
	bool m_bReceiverStarted;
	std::shared_ptr<std::thread> m_thread;
};

