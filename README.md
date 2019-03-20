# evalDB

Use your favorite language as a database.

![transactin rollbacks on error](https://i.imgur.com/OdgPEM6.gif)
![inserting query parameters into a table](https://i.imgur.com/xR4aBCM.gif)

Try it out: [evaldb.turb.io](https://evaldb.turb.io).

:warning: This is a **really** fragile experiment, please don't actually use it.
May break during some phases of the moon.
:warning:

## Building

The current language drivers make heavy use of Linux syscalls for snapshotting.
This probably won't work on any other OS, or if your Linux is configured even
slightly differently things might explode.

1.  Make sure you have `libjansson` and a reasonable compiler toolchain
    installed. On ubuntu `apt-get install libjansson-dev build-essential` is
    probably all you need.
2.  Run `go get ./cmd/gateway`
3.  Run `make` in the repo's root
4.  Start the server with `./gateway -path ./db -port 5000`

You can also run this in docker but we'll need to give the container extra
privileges:

1.  Build with: `docker build -t evaldb .`
2.  Start with: `docker run -p 5000:5000/tcp --privileged evaldb`

## Usage

Once your server is up and running you can now visit `http://localhost:5000`
for a fancy web UI.

Interacting with evalDB through the gateway:

#### Creating a new database:

```shell
curl localhost:5000/create -d 'name=mynewdb&lang=luaval'
```

`name` is the name used in all future queries of this database. The name
can be any alphanumeric string with dashes in-between.

`lang` is the language this database will run. The options are:

- `luaval` for lua
- `duktape` for javascript.

#### A simple query:

```shell
curl localhost:5000/eval/mynewdb -H 'Content-Type: application/json' -d '{"code":"return 1 + 1"}'
```

Which returns returns:

```json
{ "object": 2, "warm": false, "walltime": 52235314, "gen": 1, "parent": 0 }
```

## How it works

Every database is owned and controlled by an evaler process. A gateway
in-between manages these evalers and makes them easy to talk to.

```
+-----+  http  +---------+  stdio  +--------+
| you | <----> | gateway | <-----> | evaler |
+-----+        +---------+         +--------+
```

The evaler process is where all the magic happens. Every evaler has a memory
mapped file where the interpreter's entire heap is stored. The heap is versioned
through a copy-on-write mechanism where every generation holds a list of pages
modified since its parent generation.

The evaler receives queries as json over stdin and responds to queries with
json over stdout.

This looks something like:

```
            ^
            | stdio
            v
+----------------------+
|        driver        |
+----------------------+
| language interpreter |
+----------------------+
|      allocator       |
+----------------------+
           ^
           | mmaped io
           v
+----------------------+
|         file         |
+----------------------+
```

A query is generally processed like this:

1.  The driver reads a query as json from stdin.
2.  The driver builds a function using the query's `code` and `args`.
3.  The driver tells the allocator to checkout the appropriate generation and
    create a new child generation.
4.  The function is evaluated in the target language's interpreter.
5.  every memory write by the interpreter is trapped by the allocator creating a
    new page owned by the current generation.
6.  The result of the eval is checked
    - on error: the current generation is abandoned and the error is returned.
    - on success: the current generation is committed and a json representation of
      the result is returned.
