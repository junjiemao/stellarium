/*
 * Stellarium Telescope Control Plug-in
 * 
 * Copyright (C) 2009 Bogdan Marinov (this file,
 * reusing code written by Johannes Gajdosik in 2006)
 * 
 * Johannes Gajdosik wrote in 2006 the original telescope control feature
 * as a core module of Stellarium. In 2009 it was significantly extended with
 * GUI features and later split as an external plug-in module by Bogdan Marinov.
 * 
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Suite 500, Boston, MA  02110-1335, USA.
 */
 
#include <QRegularExpression>
#include <QStringList>

#include "TelescopeClientDirectLx200.hpp"

#include "Lx200Connection.hpp"
#include "Lx200Command.hpp"
#include "common/LogFile.hpp"
#include "StelCore.hpp"
#ifndef QT_NO_DEBUG
#include "StelUtils.hpp"
#endif


TelescopeClientDirectLx200::TelescopeClientDirectLx200 (const QString &name, const QString &parameters, TelescopeControl::Equinox eq)
	: TelescopeClient(name)
	, time_delay(0)
	, equinox(eq)
	, lx200(nullptr)
	, long_format_used(false)
	, answers_received(false)
	, last_ra(0)
	, queue_get_position(true)
	, next_pos_time(0)
{
	interpolatedPosition.reset();
	
	//Extract parameters
	//Format: "serial_port_name:time_delay"
	static const QRegularExpression paramRx("^([^:]*):(\\d+)$");
	QRegularExpressionMatch paramMatch=paramRx.match(parameters);
	QString serialDeviceName;
	if (paramMatch.hasMatch())
	{
		// This RegExp only matches valid integers
		serialDeviceName = paramMatch.captured(1).trimmed();
		time_delay       = paramMatch.captured(2).toInt();
	}
	else
	{
		qWarning() << "ERROR creating TelescopeClientDirectLx200: invalid parameters.";
		return;
	}
	
	qDebug() << "TelescopeClientDirectLx200 parameters: port, time_delay:" << serialDeviceName << time_delay;
	
	//Validation: Time delay
	if (time_delay <= 0 || time_delay > 10000000)
	{
		qWarning() << "ERROR creating TelescopeClientDirectLx200: time_delay not valid (should be less than 10000000)";
		return;
	}
	
	//end_of_timeout = -0x8000000000000000LL;
	
	#ifdef Q_OS_WIN
	if(serialDeviceName.right(serialDeviceName.size() - 3).toInt() > 9)
		serialDeviceName = "\\\\.\\" + serialDeviceName;//"\\.\COMxx", not sure if it will work
	#endif //Q_OS_WIN
	
	//Try to establish a connection to the telescope
	lx200 = new Lx200Connection(*this, qPrintable(serialDeviceName));
	if (lx200->isClosed())
	{
		qWarning() << "ERROR creating TelescopeClientDirectLx200: cannot open serial device" << serialDeviceName;
		return;
	}
	
	// lx200 will be deleted in the destructor of Server
	addConnection(lx200);
	
	long_format_used = false; // unknown
	last_ra = 0;
	queue_get_position = true;
	next_pos_time = -0x8000000000000000LL;
	answers_received = false;
}

//! queues a GOTO command
void TelescopeClientDirectLx200::telescopeGoto(const Vec3d &j2000Pos, StelObjectP selectObject)
{
	Q_UNUSED(selectObject)

	if (!isConnected())
		return;

	Vec3d position = j2000Pos;
	if (equinox == TelescopeControl::EquinoxJNow)
	{
		const StelCore* core = StelApp::getInstance().getCore();
		position = core->j2000ToEquinoxEqu(j2000Pos, StelCore::RefractionOff);
	}

	const double ra_signed = atan2(position[1], position[0]);
	//Workaround for the discrepancy in precision between Windows/Linux/PPC Macs and Intel Macs:
	const double ra = (ra_signed >= 0) ? ra_signed : (ra_signed + 2.0 * M_PI);
	const double dec = atan2(position[2], std::sqrt(position[0]*position[0]+position[1]*position[1]));
	unsigned int ra_int = static_cast<unsigned int>(floor(0.5 + ra*(static_cast<unsigned int>(0x80000000)/M_PI)));
	int dec_int = static_cast<int>(floor(0.5 + dec*(static_cast<unsigned int>(0x80000000)/M_PI)));

	gotoReceived(ra_int, dec_int);
}

void TelescopeClientDirectLx200::telescopeSync(const Vec3d &j2000Pos, StelObjectP selectObject)
{
	Q_UNUSED(selectObject)

	if (!isConnected())
		return;

	Vec3d position = j2000Pos;
	if (equinox == TelescopeControl::EquinoxJNow)
	{
		const StelCore* core = StelApp::getInstance().getCore();
		position = core->j2000ToEquinoxEqu(j2000Pos, StelCore::RefractionOff);
	}

	const double ra_signed = atan2(position[1], position[0]);
	//Workaround for the discrepancy in precision between Windows/Linux/PPC Macs and Intel Macs:
	const double ra = (ra_signed >= 0) ? ra_signed : (ra_signed + 2.0 * M_PI);
	const double dec = atan2(position[2], std::sqrt(position[0]*position[0]+position[1]*position[1]));
	unsigned int ra_int = static_cast<unsigned int>(floor(0.5 + ra*(static_cast<unsigned int>(0x80000000)/M_PI)));
	int dec_int = static_cast<int>(floor(0.5 + dec*(static_cast<unsigned int>(0x80000000)/M_PI)));

	syncReceived(ra_int, dec_int);
}

void TelescopeClientDirectLx200::gotoReceived(unsigned int ra_int, int dec_int)
{
	lx200->sendGoto(ra_int, dec_int);
}

void TelescopeClientDirectLx200::syncReceived(unsigned int ra_int, int dec_int)
{
	lx200->sendSync(ra_int, dec_int);
}


//! estimates where the telescope is by interpolation in the stored
//! telescope positions:
Vec3d TelescopeClientDirectLx200::getJ2000EquatorialPos(const StelCore*) const
{
	const qint64 now = getNow() - time_delay;
	return interpolatedPosition.get(now);
}

bool TelescopeClientDirectLx200::prepareCommunication()
{
	//TODO: Nothing to prepare?
	return true;
}

void TelescopeClientDirectLx200::performCommunication()
{
	step(10000);
}

void TelescopeClientDirectLx200::communicationResetReceived(void)
{
	long_format_used = false;
	queue_get_position = true;
	next_pos_time = -0x8000000000000000LL;
	
#ifndef QT_NO_DEBUG
	*log_file << Now() << "TelescopeClientDirectLx200::communicationResetReceived" << StelUtils::getEndLineChar();
#endif

	if (answers_received)
	{
		closeAcceptedConnections();
		answers_received = false;
	}
}

//! Called in Lx200CommandGetRa and Lx200CommandGetDec.
void TelescopeClientDirectLx200::longFormatUsedReceived(bool long_format)
{
	answers_received = true;
	if (!long_format_used && !long_format)
	{
		lx200->sendCommand(new Lx200CommandToggleFormat(*this));
	}
	long_format_used = true;
}

//! Called by Lx200CommandGetRa::readAnswerFromBuffer().
void TelescopeClientDirectLx200::raReceived(unsigned int ra_int)
{
	answers_received = true;
	last_ra = ra_int;
#ifndef QT_NO_DEBUG
	*log_file << Now() << "TelescopeClientDirectLx200::raReceived: " << ra_int << StelUtils::getEndLineChar();
#endif
}

//! Called by Lx200CommandGetDec::readAnswerFromBuffer().
//! Should be called after raReceived(), as it contains a call to sendPosition().
void TelescopeClientDirectLx200::decReceived(unsigned int dec_int)
{
	answers_received = true;
#ifndef QT_NO_DEBUG
	*log_file << Now() << "TelescopeClientDirectLx200::decReceived: " << dec_int << StelUtils::getEndLineChar();
#endif
	const int lx200_status = 0;
	sendPosition(last_ra, static_cast<int>(dec_int), lx200_status);
	queue_get_position = true;
}

void TelescopeClientDirectLx200::step(long long int timeout_micros)
{
	long long int now = GetNow();
	if (queue_get_position && now >= next_pos_time)
	{
		lx200->sendCommand(new Lx200CommandGetRa(*this));
		lx200->sendCommand(new Lx200CommandGetDec(*this));
		queue_get_position = false;
		next_pos_time = now + 500000;// 500000;
	}
	Server::step(timeout_micros);
}

bool TelescopeClientDirectLx200::isConnected(void) const
{
	return (!lx200->isClosed());//TODO
}

bool TelescopeClientDirectLx200::isInitialized(void) const
{
	return (!lx200->isClosed());
}

//Merged from Connection::sendPosition() and TelescopeTCP::performReading()
void TelescopeClientDirectLx200::sendPosition(unsigned int ra_int, int dec_int, int status)
{
	//Server time is "now", because this class is the server
	const qint64 server_micros = static_cast<qint64>(getNow());
	const double ra  =  ra_int * (M_PI/0x80000000u);
	const double dec = dec_int * (M_PI/0x80000000u);
	const double cdec = cos(dec);
	Vec3d position(cos(ra)*cdec, sin(ra)*cdec, sin(dec));
	Vec3d j2000Position = position;
	if (equinox == TelescopeControl::EquinoxJNow)
	{
		const StelCore* core = StelApp::getInstance().getCore();
		j2000Position = core->equinoxEquToJ2000(position, StelCore::RefractionOff);
	}
	interpolatedPosition.add(j2000Position, getNow(), server_micros, status);
}
