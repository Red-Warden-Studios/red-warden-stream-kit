/*
Red Warden In-Flight
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "inflight-probe-logic.hpp"

#include <QString>

namespace inflight {

bool anyDestinationLive(const QStringList &states)
{
	for (const QString &s : states)
		if (s.compare(QLatin1String("live"), Qt::CaseInsensitive) == 0 ||
		    s.compare(QLatin1String("reconnecting"), Qt::CaseInsensitive) == 0)
			return true;
	return false;
}

LiveState combineLive(bool mainStreamActive, const MsReply &ms, qint64 nowMs)
{
	if (mainStreamActive)
		return LiveState::Live;
	if (ms.status == MsStatus::NotFound)
		return LiveState::Off;
	if (ms.status != MsStatus::Ok)
		return LiveState::Unknown; // Pending or Error
	if (nowMs < ms.atMs || nowMs - ms.atMs > kMsFreshMs)
		return LiveState::Unknown;
	return ms.anyLive ? LiveState::Live : LiveState::Off;
}

StreamEventKind stopKindForCode(int code)
{
	return code == kOutputSuccess ? StreamEventKind::StoppedOk : StreamEventKind::StoppedError;
}

} // namespace inflight
