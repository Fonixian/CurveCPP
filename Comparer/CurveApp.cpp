#define SDL_MAIN_USE_CALLBACKS
#include <SDL3/SDL_main.h>
#include "app.h"

SDL_AppResult SDL_AppInit(void** appstate, int argc, char** argv)
{
	App* app = new App();
	SDL_AppResult result = app->Init();
	*appstate = app;
	return result;
	return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void* appstate)
{
	static Uint64 old_ns = SDL_GetTicksNS();
	Uint64 new_ns = SDL_GetTicksNS();
	float delta = float(new_ns - old_ns) / 1000000000;
	old_ns = new_ns;
	App* app = static_cast<App*> (appstate);
	return app->Iterate(delta);
}

SDL_AppResult SDL_AppEvent(void* appstate, SDL_Event* Event)
{
	App* app = static_cast<App*> (appstate);
	return app->Event(*Event);
	return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void* appstate, SDL_AppResult result)
{
	App* app = static_cast<App*> (appstate);
	app->Quit(result);
	delete app;
}