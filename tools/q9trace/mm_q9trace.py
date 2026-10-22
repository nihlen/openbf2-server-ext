# [TEST ONLY] ModManager module for the Q9 runs (docs/findings/q5-q9.md). Never deploy it.
#
# The server swallows an exception raised by a game event handler: PythonHost::handleEvent calls
# PyErr_Clear and logs nothing (OpenBF2 Code/BF2/Game/Python/PythonHost.cpp:815-822), as retail
# does. This module wraps every handler registered after it loads, so such exceptions are written
# to q9trace.log in the server's working directory and then re-raised unchanged. It also logs each
# call of a handler for the events in CALLS, so the log shows which handlers the engine ran (a handler
# registered without alwaysCall runs only while the round is Running, PythonHost.cpp:805-813). Load it first:
#   modmanager.loadModule "mm_q9trace"     (before every other loadModule line)
# Handlers registered before ModManager loads (the game mode's init-time ones) are not covered.
# Python 2.3: no f-strings, no traceback.format_exc.

import host
import sys
import time
import traceback

__version__ = 1.0
__required_modules__ = {'modmanager': 1.2}
__supports_reload__ = False
__supported_games__ = {'bf2': True}
__description__ = "Q9 handler exception tracer v%s (test only)" % __version__

LOG = 'q9trace.log'
CALLS = ['PlayerConnect', 'PlayerSpawn', 'PlayerKilled', 'PlayerDeath', 'PlayerScore', 'PlayerChangeTeams',
         'EnterVehicle', 'ExitVehicle', 'VehicleDestroyed', 'PlayerKicked', 'PlayerBanned', 'PlayerDisconnect',
         'GameStatus']


def _describe(a):
    try:
        if hasattr(a, 'index') and hasattr(a, 'getName'):
            return 'player %d' % a.index
        if hasattr(a, 'templateName'):
            return 'object %s' % a.templateName
    except:
        pass
    return repr(a)


def _log(msg):
    f = open(LOG, 'a')
    f.write('%s %s\n' % (time.strftime('%H:%M:%S'), msg))
    f.close()


def _wrap(kind, cb):
    name = getattr(cb, '__name__', repr(cb))
    module = getattr(cb, '__module__', '?')

    def wrapped(*args):
        if kind in CALLS:
            _log('call %s %s.%s(%s)' % (kind, module, name, ', '.join(map(_describe, args))))
        try:
            return cb(*args)
        except:
            t, v, tb = sys.exc_info()
            _log('%s handler %s.%s raised:\n%s' % (kind, module, name,
                                                   ''.join(traceback.format_exception(t, v, tb))))
            raise
    return wrapped


_registerHandler = host.registerHandler
_registerGameStatusHandler = host.registerGameStatusHandler


def registerHandler(event, cb, alwaysCall=0):
    return _registerHandler(event, _wrap(event, cb), alwaysCall)


def registerGameStatusHandler(cb):
    return _registerGameStatusHandler(_wrap('GameStatus', cb))


host.registerHandler = registerHandler
host.registerGameStatusHandler = registerGameStatusHandler
_log('q9trace loaded; wrapping handlers registered from now on')


class Q9Trace(object):
    def __init__(self, modManager):
        self.mm = modManager

    def init(self):
        self.mm.info("Q9 trace: handler exceptions go to %s" % LOG)

    def shutdown(self):
        pass


def mm_load(modManager):
    return Q9Trace(modManager)
