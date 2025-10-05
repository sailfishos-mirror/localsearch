# Removable devices

If configured to index removable devices, LocalSearch will monitor the removable
volumes added and removed, and index the files available in these devices in
addition to the configured directories.

The indexed data will be stored in the devices themselves, in a folder called
`.localsearch3` in the root folder of the mountpoint. The indexed data will be
available for access through a D-Bus SPARQL endpoint created at the D-Bus path
`/org/freedesktop/LocalSearch3/...` where the last component of the path is
the mountpoint path with non-alphanumeric characters encoded as `_%x` of the
ASCII codepoints.

```python
#!/bin/env python3
import gi
gi.require_version('Tsparql', '3.0')
from gi.repository import Tsparql

def removable_device_object_path(path):
    object_path = ''
    for c in path:
        if (c >= 'a' and c <= 'z') or
           (c >= 'A' and c <= 'Z') or
           (c >= '0' and c <= '9'):
            object_path += c
        else:
            object_path += '_%x' % ord(c)
    return '/org/freedesktop/LocalSearch3/%s' % object_path

object_path = removable_device_object_path(mountpoint)
conn = Tsparql.SparqlConnection.bus_new(
    'org.freedesktop.LocalSearch3', object_path)
```

Multiple endpoints may be created for multiple removable device mountpoints.

These dedicated endpoints for removable devices follow the same specifications
and restrictions than [the default endpoint](endpoint.md). With the only
difference that URIs are expressed in a relative `file:` URI scheme, with
`file:` representing the root directory of the mountpoint, and e.g. `file:a.txt`
representing a file called `a.txt` in it.

Users may track the existence of these endpoints through the `'EndpointAdded`
and `'EndpointRemoved` D-Bus signals emitted through the interface
`org.freedesktop.Tracker3.Miner`.
