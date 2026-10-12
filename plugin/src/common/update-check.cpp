/*
Red Warden Stream Kit
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "update-check.hpp"

#include <QMetaObject>

#include <atomic>
#include <chrono>
#include <mutex>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#endif

struct UpdateChecker::Shared {
	std::mutex m;
	bool cancelled = false;       // guarded by m: once true, the worker never posts
	std::atomic<bool> quit{false}; // lock-free copy for the read loop
	std::atomic<bool> done{false};
};

namespace {
#ifdef _WIN32
// One shared Stream Kit manifest (see update-manifest.hpp), for both plugins.
static const std::wstring kHost = QString::fromLatin1(updatecheck::kKitHost).toStdWString();
static const std::wstring kPath = QString::fromLatin1(updatecheck::kKitManifestPath).toStdWString();
constexpr int kTimeoutMs = 3000;       // per phase (resolve, connect, send, receive)
constexpr DWORD kMaxBytes = 64 * 1024; // the manifest is a few hundred bytes

// One HTTPS GET. Empty on any failure. Runs on the worker thread, which alone
// opens and closes every handle here.
QByteArray fetch(const QString &version, const std::atomic<bool> &quit)
{
	const std::wstring agent = L"RedWardenStreamKit/" + version.toStdWString();
	HINTERNET session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
					WINHTTP_NO_PROXY_BYPASS, 0);
	if (!session)
		return {};
	WinHttpSetTimeouts(session, kTimeoutMs, kTimeoutMs, kTimeoutMs, kTimeoutMs);
	QByteArray body;
	HINTERNET conn = WinHttpConnect(session, kHost.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
	HINTERNET req = conn ? WinHttpOpenRequest(conn, L"GET", kPath.c_str(), nullptr, WINHTTP_NO_REFERER,
						  WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
			     : nullptr;
	if (req && !quit.load() &&
	    WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
	    WinHttpReceiveResponse(req, nullptr)) {
		DWORD status = 0, size = sizeof(status);
		WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
				    WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
		char buf[4096];
		DWORD got = 0;
		while (status == 200 && !quit.load() && WinHttpReadData(req, buf, sizeof(buf), &got) && got > 0) {
			body.append(buf, (int)got);
			if ((DWORD)body.size() > kMaxBytes) {
				body.clear();
				break;
			}
		}
		if (status != 200)
			body.clear();
	}
	if (req)
		WinHttpCloseHandle(req);
	if (conn)
		WinHttpCloseHandle(conn);
	WinHttpCloseHandle(session);
	return body;
}

// Keep this DLL mapped until the process exits, so a detached worker's code
// can't be unloaded out from under it.
void pinThisModule()
{
	HMODULE self = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
			   reinterpret_cast<LPCWSTR>(&pinThisModule), &self);
}
#endif
} // namespace

UpdateChecker::UpdateChecker(QObject *parent) : QObject(parent) {}

UpdateChecker::~UpdateChecker()
{
	stop();
}

void UpdateChecker::start(const QString &currentVersion)
{
#ifdef _WIN32
	if (shared)
		return; // once per OBS start
	shared = std::make_shared<Shared>();
	std::shared_ptr<Shared> sh = shared;
	worker = std::thread([this, sh, currentVersion] {
		const QByteArray body = fetch(currentVersion, sh->quit);
		updatecheck::Manifest m;
		const bool newer = !body.isEmpty() && updatecheck::parseManifest(body, &m) &&
				   updatecheck::compareVersions(m.latest, currentVersion) > 0;
		{
			// Posting under the lock: stop() takes the same lock to cancel, so
			// once stop() has returned this can never touch the checker again.
			std::lock_guard<std::mutex> lock(sh->m);
			if (newer && !sh->cancelled)
				QMetaObject::invokeMethod(
					this, [this, m] { emit updateAvailable(m); }, Qt::QueuedConnection);
		}
		sh->done = true;
	});
#else
	Q_UNUSED(currentVersion);
#endif
}

void UpdateChecker::stop()
{
	if (!shared)
		return;
	{
		std::lock_guard<std::mutex> lock(shared->m);
		shared->cancelled = true;
	}
	shared->quit = true;
	if (!worker.joinable())
		return;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
	while (!shared->done.load() && std::chrono::steady_clock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	if (shared->done.load()) {
		worker.join();
	} else {
#ifdef _WIN32
		pinThisModule(); // the worker is stuck in the network; never hang OBS's exit on it
#endif
		worker.detach();
	}
}
