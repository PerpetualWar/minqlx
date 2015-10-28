#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "quake_common.h"
#include "maps_parser.h"

int is_fake_admin = 0;

/* Takes a 64-bit integer used as a bit field as flags for which player
 * has an action pending, removes the flag and returns the client ID.
 * The server only allows up to 64 players, so a 64-bit int covers it all.
 * 
 * Returns -1 if no flag is set, so use it in a loop until it does so. */
int GetPendingPlayer(uint64_t* players) {
    int flag = -1;
    // We first check if any bitfield is set.
    if (!*players) return flag;
    else {
        for (int id = 0; id < 64; id++) {
            // Check bit i's flag.
            flag = *players & (1LL << id);
            // Remove the flag we checked, if present.
            *players &= ~flag;
            // If the flag was set, return client id.
            if (flag) return id;
        }
    }
    
    return -1; // All flags have been cleared.
}

// Set a flag on client ID to indicate a pending action on the player.
void SetPendingPlayer(uint64_t* players, int client_id) {
    *players |= 1LL << client_id;
}

int ExecuteAdminCommand(int client_id, const char* args) {
	Cmd_TokenizeString(args);
	char* cmd = Cmd_Argv(0);
	gentity_t* executor;
	if (client_id == -1)
		executor = &fake_entity;
	else
		executor = &g_entities[client_id];

	for (adminCmd_t* ac = admin_commands; ac->cmd; ac++) {
		if (!strcmp(ac->cmd, cmd)) {
			if (client_id == -1 && (!strcmp(ac->cmd, "opsay") || !strcmp(ac->cmd, "abort") ||
				!strcmp(ac->cmd, "pause") || !strcmp(ac->cmd, "unpause") || !strcmp(ac->cmd, "timeout") ||
				!strcmp(ac->cmd, "put") || !strcmp(ac->cmd, "timein")))
			{
				// The above commands prints the caller's name, so we tell our hooked "GetClientName"
				// to print "An admin" instead when we use a fake client. Otherwise it'll try
				// to access the name of a nonexsistent player and segfault.
				is_fake_admin = 1;
				ac->admin_func(executor);
				is_fake_admin = 0;
			}
			else {
				ac->admin_func(executor);
			}
			return 0;
		}
	}

	return 1;
}

// (0.0f, 1.0f)
float RandomFloat(void) {
      return (float)rand()/(float)RAND_MAX;
}

// (-1.0f, 1.0f)
float RandomFloatWithNegative(void) {
      return (float)rand()/(float)(RAND_MAX/2) - 1;
}

void* PatternSearch(void* address, size_t length, const char* pattern, const char* mask) {
  for (size_t i = 0; i < length; i++) {
    for (size_t j = 0; mask[j]; j++) {
      if (mask[j] == 'X' && pattern[j] != ((char*)address)[i + j]) {
        break;
      }
      else if (mask[j + 1]) {
        continue;
      }

      return (void*)(((pint)address) + i);
    }
  }
  return NULL;
}

void* PatternSearchModule(module_info_t* module, const char* pattern, const char* mask) {
	void* res = NULL;
	for (int i = 0; i < module->entries; i++) {
		if (!(module->permissions[i] & PG_READ)) continue;
		size_t size = module->address_end[i] - module->address_start[i];
		res = PatternSearch((void*)module->address_start[i], size, pattern, mask);
		if (res) break;
	}

	return res;
}

