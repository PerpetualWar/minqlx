# minqlx - Extends Quake Live's dedicated server with extra functionality and scripting.
# Copyright (C) 2015 Mino <mino@minomino.org>

# This file is part of minqlx.

# minqlx is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.

# minqlx is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.

# You should have received a copy of the GNU General Public License
# along with minqlx. If not, see <http://www.gnu.org/licenses/>.

import minqlx
import re

re_color_tag = re.compile(r"\^[^\^]")

# ====================================================================
#                              COMMANDS
# ====================================================================

class Command:
    """A class representing a input-triggered command.

    Has information about the command itself, its usage, when and who to call when
    action should be taken.

    """
    def __init__(self, plugin, name, handler, permission, channels, exclude_channels, client_cmd_pass, client_cmd_perm, prefix, usage):
        if not (channels == None or hasattr(channels, "__iter__")):
            raise ValueError("'channels' must be a finite iterable or None.")
        elif not (channels == None or hasattr(exclude_channels, "__iter__")):
            raise ValueError("'exclude_channels' must be a finite iterable or None.")
        self.plugin = plugin # Instance of the owner.

        # Allow a command to have alternative names.
        if isinstance(name, list) or isinstance(name, tuple):
            self.name = [n.lower() for n in name]
        else:
            self.name = [name]
        self.handler = handler
        self.permission = permission
        self.channels = list(channels) if channels != None else []
        self.exclude_channels = list(exclude_channels) if exclude_channels != None else []
        self.client_cmd_pass = client_cmd_pass
        self.client_cmd_perm = client_cmd_perm
        self.prefix = prefix
        self.usage = usage

    def execute(self, player, msg, channel):
        logger = minqlx.get_logger(self.plugin)
        logger.debug("{} executed: {} @ {} -> {}"
            .format(player.steam_id, self.name[0], self.plugin.name, channel))
        return self.handler(player, msg.split(), channel)

    def is_eligible_name(self, name):
        if self.prefix:
            if name[0] != minqlx.command_prefix():
                return False
            name = name[1:]
        
        return name.lower() in self.name

    def is_eligible_channel(self, channel):
        """Check if a chat channel is one this command should execute in.

        Exclude takes precedence.
        
        """
        if channel in self.exclude_channels:
            return False
        elif not self.channels or channel.name in self.channels:
            return True
        else:
            return False

    def is_eligible_player(self, player, is_client_cmd):
        """Check if a player has the rights to execute the command."""
        # Check if config overrides permission.
        conf = minqlx.get_config()
        perm = self.permission
        client_cmd_perm = self.client_cmd_perm
        perm_key = "perm_" + self.name[0]
        client_cmd_perm_key = "client_cmd_perm_" + self.name[0]
        if self.plugin.name in conf:
            if perm_key in conf[self.plugin.name]:
                perm = int(conf[self.plugin.name][perm_key])
            if is_client_cmd and client_cmd_perm_key in conf[self.plugin.name]:
                client_cmd_perm = int(conf[self.plugin.name][client_cmd_perm_key])

        if (player.steam_id == minqlx.owner() or
            (not is_client_cmd and perm == 0) or
            (is_client_cmd and client_cmd_perm == 0)):
            return True
        
        player_perm = self.plugin.db.get_permission(player)
        if is_client_cmd:
            return player_perm >= client_cmd_perm
        else:
            return player_perm >= perm

class CommandInvoker:
    """Holds all commands and executes them whenever we get input and should execute.

    """
    def __init__(self):
        self._commands = ([], [], [], [], [])

    @property
    def commands(self):
        c = []
        for cmds in self._commands:
            c.extend(cmds)

        return c

    def add_command(self, command, priority):
        if self.is_registered(command):
            raise ValueError("Attempted to add an already registered command.")
        
        self._commands[priority].append(command)

    def remove_command(self, command):
        if not self.is_registered(command):
            raise ValueError("Attempted to remove a command that was never added.")
        else:
            for priority_level in self._commands:
                for cmd in priority_level:
                    if cmd == command:
                        priority_level.remove(cmd)
                        return

    def is_registered(self, command):
        """Check if a command is already registed.

        Commands are unique by (command.name, command.handler).

        """
        for priority_level in self._commands:
            for cmd in priority_level:
                if command.name == cmd.name and command.handler == cmd.handler:
                    return True

        return False

    def handle_input(self, player, msg, channel):
        if not msg.strip():
            return

        name = msg.split(" ", 1)[0].lower()
        is_client_cmd = channel == "client_command"
        pass_through = True
        
        for priority_level in self._commands:
            for cmd in priority_level:
                if cmd.is_eligible_name(name) and cmd.is_eligible_channel(channel) and cmd.is_eligible_player(player, is_client_cmd):
                    # Client commands will not pass through to the engine unless told to explicitly.
                    # This is to avoid having to return RET_STOP_EVENT just to not get the "unknown cmd" msg.
                    if is_client_cmd:
                        pass_through = cmd.client_cmd_pass

                    # Dispatch "command" and allow people to stop it from being executed.
                    if minqlx.EVENT_DISPATCHERS["command"].dispatch(player, cmd, msg) == False:
                        return True
                    
                    res = cmd.execute(player, msg, channel)
                    if res == minqlx.RET_STOP:
                        return
                    elif res == minqlx.RET_STOP_EVENT:
                        pass_through = False
                    elif res == minqlx.RET_STOP_ALL:
                        # C-level dispatchers expect False if it shouldn't go to the engine.
                        return False
                    elif res == minqlx.RET_USAGE and cmd.usage:
                        channel.reply("^7Usage: ^6{} {}".format(name, cmd.usage))
                    elif res != None and res != minqlx.RET_NONE:
                        logger = minqlx.get_logger(None)
                        logger.warning("Command '{}' with handler '{}' returned an unknown return value: {}"
                            .format(cmd.name, cmd.handler.__name__, res))

        return pass_through

# ====================================================================
#                             CHANNELS
# ====================================================================

class AbstractChannel:
    """An abstract class of a chat channel. A chat channel being a source of a message.

    Chat channels must implement reply(), since that's the whole point of having a chat channel
    as a class. Makes it quite convenient when dealing with commands and such, while allowing
    people to implement their own channels, opening the possibilites for communication with the
    bot through other means than just chat and console (e.g. web interface).

    Say "ChatChannelA" and "ChatChannelB" are both subclasses of this, and "cca" and "ccb" are instances,
    the default implementation of "cca == ccb" is comparing __repr__(). However, when you register
    a command and list what channels you want it to work with, it'll use this class' __str__(). It's
    important to keep this in mind if you make a subclass. Say you have a web interface that
    supports multiple users on it simulaneously. The right way would be to set "name" to something
    like "webinterface", and then implement a __repr__() to return something like "webinterface user1".
    
    """
    def __init__(self, name):
        self._name = name

    def __str__(self):
        return self.name

    def __repr__(self):
        return str(self)

    # Equal.
    def __eq__(self, other):
        if isinstance(other, str):
            # For string comparison, we use self.name. This allows
            # stuff like: if channel == "tell": do_something()
            return self.name == other
        else:
            return repr(self) == repr(other)

    # Not equal.
    def __ne__(self, other):
        return not self.__eq__(other)

    @property
    def name(self):
        return self._name

    def reply(self):
        raise NotImplementedError()

    def split_long_msg(self, msg, limit=100, delimiter=" "):
        """Split a message into several pieces for channels with limtations."""
        if len(msg) < limit:
            return [msg]
        out = []
        index = limit
        for i in reversed(range(limit)):
            if msg[i:i + len(delimiter)] == delimiter:
                index = i
                out.append(msg[0:index])
                # Keep going, but skip the delimiter.
                rest = msg[index + len(delimiter):]
                if rest:
                    out.extend(self.split_long_msg(rest, limit, delimiter))
                return out

        out.append(msg[0:index])
        # Keep going.
        rest = msg[index:]
        if rest:
            out.extend(self.split_long_msg(rest, limit, delimiter))
        return out

class ChatChannel(AbstractChannel):
    """A channel for chat to and from the server."""
    def __init__(self):
        super().__init__("chat")
        self.fmt = "print \"{}\n\"\n"
        self.team = "all"
    
    @minqlx.next_frame
    def reply(self, msg):
        msg = str(msg)
        # Can deal with all the below ChatChannel subclasses.
        last_color = ""
        targets = None

        if isinstance(self, minqlx.TellChannel):
            cid = minqlx.Plugin.client_id(self.recipient)
            if cid == None:
                raise ValueError("Invalid recipient.")
            targets = [cid]
        elif self.team != "all":
            targets = []
            for p in minqlx.Player.all_players():
                if p.team == self.team:
                    targets.append(p.id)
        
        for s in self.split_long_msg(msg, limit=100):
            if not targets:
                minqlx.send_server_command(None, self.fmt.format(last_color + s))
            else:
                for cid in targets:
                    minqlx.send_server_command(cid, self.fmt.format(last_color + s))
            
            find = re_color_tag.findall(s)
            if find:
                last_color = find[-1]

class RedTeamChatChannel(ChatChannel):
    """A channel for in-game chat to and from the red team."""
    def __init__(self):
        super(ChatChannel, self).__init__("red_team_chat")
        self.fmt = "print \"{}\n\"\n"
        self.team = "red"

    def reply(self, msg):
        super().reply(msg)

class BlueTeamChatChannel(ChatChannel):
    """A channel for in-game chat to and from the blue team."""
    def __init__(self):
        super(ChatChannel, self).__init__("blue_team_chat")
        self.fmt = "print \"{}\n\"\n"
        self.team = "blue"

    def reply(self, msg):
        super().reply(msg)

class FreeChatChannel(ChatChannel):
    """A channel for in-game chat to and from the free team. The free team
    is the team all FFA players are in.

    """
    def __init__(self):
        super(ChatChannel, self).__init__("free_chat")
        self.fmt = "print \"{}\n\"\n"
        self.team = "free"

    def reply(self, msg):
        super().reply(msg)

class SpectatorChatChannel(ChatChannel):
    """A channel for in-game chat to and from the spectator team."""
    def __init__(self):
        super(ChatChannel, self).__init__("spectator_chat")
        self.fmt = "print \"{}\n\"\n"
        self.team = "spectator"

    def reply(self, msg):
        super().reply(msg)

class TellChannel(ChatChannel):
    """A channel for private in-game messages."""
    def __init__(self, player):
        super(ChatChannel, self).__init__("tell")
        self.fmt = "print \"{}\n\"\n"
        self.recipient = player

    def __repr__(self):
        return "tell {}".format(minqlx.Plugin.player(self.recipient).steam_id)

    def reply(self, msg):
        super().reply(msg)

class ConsoleChannel(AbstractChannel):
    """A channel that prints to the console."""
    def __init__(self):
        super().__init__("console")

    def reply(self, msg):
        print(str(msg))

class ClientCommandChannel(AbstractChannel):
    """Wraps a TellChannel, but with its own name."""
    def __init__(self, player):
        super().__init__("client_command")
        self.recipient = player
        self.tell_channel = TellChannel(player)

    def __repr__(self):
        return "client_command {}".format(minqlx.Plugin.player(self.recipient).id)

    def reply(self, msg):
        self.tell_channel.reply(msg)


# ====================================================================
#                          MODULE CONSTANTS
# ====================================================================

COMMANDS = CommandInvoker()
CHAT_CHANNEL = ChatChannel()
RED_TEAM_CHAT_CHANNEL = RedTeamChatChannel()
BLUE_TEAM_CHAT_CHANNEL = BlueTeamChatChannel()
FREE_CHAT_CHANNEL = FreeChatChannel()
SPECTATOR_CHAT_CHANNEL = SpectatorChatChannel()
CONSOLE_CHANNEL = ConsoleChannel()
