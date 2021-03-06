#define _GNU_SOURCE
#define __STDC_FORMAT_MACROS

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <dlfcn.h>

#include "patterns.h"
#include "common.h"
#include "quake_common.h"
#include "simple_hook.h"

#ifndef NOPY
#include "pyminqlx.h"
#endif

// qagame module.
void* qagame;
void* qagame_dllentry;

static void SetTag(void);

void __cdecl My_Cmd_AddCommand(char* cmd, void* func) {
    if (!common_initialized) InitializeStatic();
    
    Cmd_AddCommand(cmd, func);
}

void __cdecl My_Sys_SetModuleOffset(char* moduleName, void* offset) {
    // We should be getting qagame, but check just in case.
    if (!strcmp(moduleName, "qagame")) {
        // Despite the name, it's not the actual module, but vmMain.
        // We use dlinfo to get the base of the module so we can properly
        // initialize all the pointers relative to the base.
    	qagame_dllentry = offset;

        Dl_info dlinfo;
        int res = dladdr(offset, &dlinfo);
        if (!res) {
            DebugError("dladdr() failed.\n", __FILE__, __LINE__, __func__);
            qagame = NULL;
        }
        else {
            qagame = dlinfo.dli_fbase;
        }
        DebugPrint("Got qagame: %#010x\n", qagame);
    }
    else
        DebugPrint("Unknown module: %s\n", moduleName);
    
    Sys_SetModuleOffset(moduleName, offset);
    if (common_initialized) {
    	SearchVmFunctions();
    	HookVm();
    	InitializeVm();
    }
}

void __cdecl My_G_InitGame(int levelTime, int randomSeed, int restart) {
    G_InitGame(levelTime, randomSeed, restart);

    if (!cvars_initialized) { // Only called once.
        SetTag();
    }
    InitializeCvars();

#ifndef NOPY
    // Only call it if we're loading a completely new game, otherwise it will also
    // be called when people ready up and the game starts.
    if (!restart)
    	NewGameDispatcher();
#endif
}

// USED FOR PYTHON

#ifndef NOPY
void __cdecl My_SV_ExecuteClientCommand(client_t *cl, const char *s, qboolean clientOK) {
    if (clientOK && cl->gentity) {
        int res = ClientCommandDispatcher(cl->gentity->s.clientNum, s);
        if (!res)
            return;
    }

    SV_ExecuteClientCommand(cl, s, clientOK);
}

void __cdecl My_SV_SendServerCommand(client_t* cl, const char* fmt, ...) {
	int res = 1;
	va_list	argptr;
	char buffer[MAX_MSGLEN];

	va_start(argptr, fmt);
	vsnprintf((char *)buffer, sizeof(buffer), fmt, argptr);
	va_end(argptr);

	if (cl && cl->gentity)
		res = ServerCommandDispatcher(cl->gentity->s.clientNum, buffer);
	else if (cl == NULL)
		res = ServerCommandDispatcher(-1, buffer);

	if (!res)
		return;

    SV_SendServerCommand(cl, buffer);
}

void __cdecl My_SV_ClientEnterWorld(client_t* client, usercmd_t* cmd) {
	clientState_t state = client->state; // State before we call real one.
	SV_ClientEnterWorld(client, cmd);

	// gentity is NULL if map changed.
	// state is CS_PRIMED only if it's the first time they connect to the server,
	// otherwise the dispatcher would also go off when a game starts and such.
	if (client->gentity != NULL && state == CS_PRIMED) {
		ClientLoadedDispatcher(client->gentity->s.clientNum);
	}
}

void  __cdecl My_G_RunFrame(int time) {
    // Dropping frames is probably not a good idea, so we don't allow cancelling.
    FrameDispatcher();

    G_RunFrame(time);
}

char* __cdecl My_ClientConnect(int clientNum, qboolean firstTime, qboolean isBot) {
	if (firstTime) {
		char* res = ClientConnectDispatcher(clientNum, isBot);
		if (res && !isBot) {
			return res;
		}
	}

	return ClientConnect(clientNum, firstTime, isBot);
}

void __cdecl My_ClientDisconnect(int clientNum) {
	ClientDisconnectDispatcher(clientNum);

	ClientDisconnect(clientNum);
}

char* __cdecl My_GetClientName(int clientNum) {
	if (is_fake_admin)
		return "An admin";

	return GetClientName(clientNum);
}

void __cdecl My_SV_SetConfigstring(int index, char* value) {
	// Indices 16 and 66X are spammed a ton every frame for some reason,
	// so we add some exceptions for those. I don't think we should have any
	// use for those particular ones anyway. If we don't do this, we get
	// like a 25% increase in CPU usage on an empty server.
	if (index == 16 || (index >= 662 && index < 670)) {
		SV_SetConfigstring(index, value);
		return;
	}

	char* res = SetConfigstringDispatcher(index, value);
	// NULL means stop the event.
	if (res)
		SV_SetConfigstring(index, res);
}
#endif

// Hook static functions. Can be done before program even runs.
void HookStatic(void) {
	int res, failed = 0;
    DebugPrint("Hooking...\n");
    res = Hook((void*)Cmd_AddCommand, My_Cmd_AddCommand, (void*)&Cmd_AddCommand);
	if (res) {
		DebugPrint("ERROR: Failed to hook Cmd_AddCommand: %d\n", res);
		failed = 1;
	}

    res = Hook((void*)Sys_SetModuleOffset, My_Sys_SetModuleOffset, (void*)&Sys_SetModuleOffset);
    if (res) {
		DebugPrint("ERROR: Failed to hook Sys_SetModuleOffset: %d\n", res);
		failed = 1;
	}

    // ==============================
    //    ONLY NEEDED FOR PYTHON
    // ==============================
#ifndef NOPY
    res = Hook((void*)SV_ExecuteClientCommand, My_SV_ExecuteClientCommand, (void*)&SV_ExecuteClientCommand);
    if (res) {
		DebugPrint("ERROR: Failed to hook SV_ExecuteClientCommand: %d\n", res);
		failed = 1;
    }

    res = Hook((void*)SV_ClientEnterWorld, My_SV_ClientEnterWorld, (void*)&SV_ClientEnterWorld);
	if (res) {
		DebugPrint("ERROR: Failed to hook SV_ClientEnterWorld: %d\n", res);
		failed = 1;
	}

	res = Hook((void*)SV_SetConfigstring, My_SV_SetConfigstring, (void*)&SV_SetConfigstring);
	if (res) {
		DebugPrint("ERROR: Failed to hook SV_SetConfigstring: %d\n", res);
		failed = 1;
	}

	res = Hook((void*)SV_SendServerCommand, My_SV_SendServerCommand, (void*)&SV_SendServerCommand);
	if (res) {
		DebugPrint("ERROR: Failed to hook SV_SendServerCommand: %d\n", res);
		failed = 1;
	}
#endif

    if (failed) {
		DebugPrint("Exiting.\n");
		exit(1);
	}
}

/* 
 * Hooks VM calls. Not all use Hook, since the VM calls are stored in a table of
 * pointers. We simply set our function pointer to the current pointer in the table and
 * then replace the it with our replacement function. Just like hooking a VMT.
 * 
 * This must be called AFTER Sys_SetModuleOffset, since Sys_SetModuleOffset is called after
 * the VM DLL has been loaded, meaning the pointer we use has been set.
 *
 * PROTIP: If you can, ALWAYS use VM_Call table hooks instead of using Hook().
*/
void HookVm(void) {
    DebugPrint("Hooking VM functions...\n");

#if defined(__x86_64__) || defined(_M_X64)
    pint vm_call_table = *(int32_t*)OFFSET_RELP_VM_CALL_TABLE + OFFSET_RELP_VM_CALL_TABLE + 4;
#elif defined(__i386) || defined(_M_IX86)
    pint vm_call_table = *(int32_t*)OFFSET_RELP_VM_CALL_TABLE + 0xCAFF4 + (pint)qagame;
#endif

	G_InitGame = *(G_InitGame_ptr*)(vm_call_table + RELOFFSET_VM_CALL_INITGAME);
	*(void**)(vm_call_table + RELOFFSET_VM_CALL_INITGAME) = My_G_InitGame;

	G_RunFrame = *(G_RunFrame_ptr*)(vm_call_table + RELOFFSET_VM_CALL_RUNFRAME);

#ifndef NOPY
	*(void**)(vm_call_table + RELOFFSET_VM_CALL_RUNFRAME) = My_G_RunFrame;

	int res, failed = 0;
	res = Hook((void*)ClientConnect, My_ClientConnect, (void*)&ClientConnect);
	if (res) {
		DebugPrint("ERROR: Failed to hook ClientConnect: %d\n", res);
		failed = 1;
	}

	res = Hook((void*)ClientDisconnect, My_ClientDisconnect, (void*)&ClientDisconnect);
	if (res) {
		DebugPrint("ERROR: Failed to hook ClientDisconnect: %d\n", res);
		failed = 1;
	}

	res = Hook((void*)GetClientName, My_GetClientName, (void*)&GetClientName);
	if (res) {
		DebugPrint("ERROR: Failed to hook GetClientName: %d\n", res);
		failed = 1;
	}

	if (failed) {
		DebugPrint("Exiting.\n");
		exit(1);
	}
#endif
}


/////////////
// HELPERS //
/////////////

static void SetTag(void) {
    // Add minqlx tag.
    char tags[1024]; // Surely 1024 is enough?
    cvar_t* sv_tags = Cvar_FindVar("sv_tags");
    if (strlen(sv_tags->string) > 2) { // Does it already have tags?
        snprintf(tags, sizeof(tags), "sv_tags \"" SV_TAGS_PREFIX ",%s\"", sv_tags->string);
        Cbuf_ExecuteText(EXEC_APPEND, tags);
    }
    else {
        Cbuf_ExecuteText(EXEC_APPEND, "sv_tags \"" SV_TAGS_PREFIX "\"");
    }
}
