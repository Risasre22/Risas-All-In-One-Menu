#include "ModexAPI.h"
// Interface code based on https://github.com/adamhynek/higgs

// Stores the API after it has already been fetched.
ModexAPI::IModexInterface001* g_ModexInterface = nullptr;

// Fetches the interface to use from Modex.
ModexAPI::IModexInterface001* ModexAPI::GetModexInterface001()
{
	if (g_ModexInterface) {
		return g_ModexInterface;
	}

	ModexMessage message;
	const auto skseMessaging = SKSE::GetMessagingInterface();
	skseMessaging->Dispatch(ModexMessage::kMessage_QueryInterface, (void*)&message,
		sizeof(ModexMessage), ModexPluginName);
	if (!message.GetApiFunction) {
		return nullptr;
	}

	g_ModexInterface = static_cast<IModexInterface001*>(message.GetApiFunction(1));
	return g_ModexInterface;
}
