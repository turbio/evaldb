package main

import (
	"bufio"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"io/ioutil"
	"log"
	"net/http"
	"os"
	"os/exec"
	"strings"
	"sync"
	"time"
)

func init() {
	log.SetFlags(log.LstdFlags | log.Lshortfile)
}

type query struct {
	Code string                 `json:"code"`
	Args map[string]interface{} `json:"args"`
	Gen  *int                   `json:"gen,omitempty"`
}

type queryResult struct {
	Object   interface{} `json:"object"`
	Error    interface{} `json:"error,omitempty"`
	Warm     bool        `json:"warm"`
	WallTime uint64      `json:"walltime"`
	Gen      int         `json:"gen"`
	Parent   int         `json:"parent"`
}

type transac struct {
	Query  query       `json:"query"`
	Result queryResult `json:"result"`
}

type evaler struct {
	db   string
	proc *os.Process
	in   io.Writer
	out  io.Reader

	sync.Mutex
}

var evalers = []*evaler{}
var elock = sync.RWMutex{}

func newEvaler(db string) (*evaler, error) {
	cmd := exec.Command("./luaval", "-d", "./db/"+db, "-s")
	cmd.Stderr = os.Stderr

	stdin, err := cmd.StdinPipe()
	if err != nil {
		return nil, err
	}

	stdout, err := cmd.StdoutPipe()
	if err != nil {
		return nil, err
	}

	err = cmd.Start()
	if err != nil {
		return nil, err
	}

	return &evaler{
		Mutex: sync.Mutex{},
		db:    db,
		proc:  cmd.Process,
		in:    stdin,
		out:   stdout,
	}, nil
}

func acquireEvaler(db string) (*evaler, bool, error) {
	elock.RLock()
	for _, e := range evalers {
		if e.db == db {
			elock.RUnlock()
			e.Lock()
			return e, false, nil
		}
	}
	elock.RUnlock()

	e, err := newEvaler(db)
	if err != nil {
		return nil, false, err
	}

	elock.Lock()
	evalers = append(evalers, e)
	elock.Unlock()

	e.Lock()
	return e, true, nil
}

func (e *evaler) release() {
	e.Unlock()
}

func must(err error) {
	if err != nil {
		log.Panicln(err)
	}
}

const MaxRequestSize = 4096

func parseQuery(w http.ResponseWriter, r *http.Request) (*query, bool) {
	ct := r.Header.Get("Content-Type")

	if strings.Contains(ct, "application/json") {
		buff, err := ioutil.ReadAll(r.Body)
		if err != nil {
			w.WriteHeader(http.StatusBadRequest)
			return nil, false
		}

		var q query
		err = json.Unmarshal(buff, &q)
		if err != nil {
			w.WriteHeader(http.StatusBadRequest)
			w.Write([]byte(err.Error()))
			return nil, false
		}

		if q.Args == nil {
			q.Args = map[string]interface{}{}
		}

		return &q, true
	}

	w.WriteHeader(http.StatusBadRequest)
	w.Write([]byte("json only"))
	return nil, false
}

func memgraph(w http.ResponseWriter, r *http.Request) {
	var renderer *exec.Cmd
	if r.URL.Query().Get("render") == "neato" {
		renderer = exec.Command("neato", "-Tsvg")
	} else {
		renderer = exec.Command("dot", "-Tsvg")
	}

	db := r.URL.Query().Get("db")

	if db == "" || !alnum(db) {
		return
	}

	grapher := exec.Command("./memgraph", "-d", "./db/"+db)

	if r.URL.Query().Get("labels") != "" {
		grapher.Args = append(grapher.Args, "-l")
	}

	if r.URL.Query().Get("segments") != "" {
		grapher.Args = append(grapher.Args, "-s")
	}

	renderer.Stdin, _ = grapher.StdoutPipe()
	renderer.Stdout = w

	w.Header().Set("content-type", "image/svg+xml")

	err := grapher.Start()
	if err != nil {
		log.Println(err)
		w.WriteHeader(500)
	}

	err = renderer.Start()
	if err != nil {
		log.Println(err)
		w.WriteHeader(500)
	}

	err = grapher.Wait()
	if err != nil {
		log.Println(err)
		w.WriteHeader(500)
	}

	err = renderer.Wait()
	if err != nil {
		log.Println(err)
		w.WriteHeader(500)
	}
}

func tail(w http.ResponseWriter, r *http.Request) {
	target := strings.TrimPrefix(r.URL.Path, "/tail/")

	w.Header().Set("Content-Type", "text/event-stream")
	f := w.(http.Flusher)

	err := tailDB(target, func(t *transac) bool {
		data, _ := json.Marshal(t)
		_, err := w.Write([]byte("event: transac\ndata: " + string(data) + "\n\n"))
		if err != nil {
			return false
		}

		f.Flush()

		return true
	})
	if err != nil {
		w.WriteHeader(http.StatusInternalServerError)
	}

	select {}
}

func eval(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Access-Control-Allow-Origin", "*")
	w.Header().Set(
		"Access-Control-Allow-Methods",
		"POST, GET, OPTIONS, PUT, DELETE",
	)

	if r.Method == "HEAD" || r.Method == "OPTIONS" {
		return
	}

	dbname := strings.TrimPrefix(r.URL.Path, "/eval/")
	if !hasDB(dbname) {
		w.WriteHeader(http.StatusBadRequest)
		w.Write([]byte("db doesn't exist"))
		return
	}

	r.Body = http.MaxBytesReader(w, r.Body, MaxRequestSize)

	q, ok := parseQuery(w, r)
	if !ok {
		return
	}

	ctx, cancel := context.WithTimeout(r.Context(), 5*time.Second)
	defer cancel()

	e, fresh, err := acquireEvaler(dbname)
	if err != nil {
		panic(err)
	}

	defer e.release()

	startAt := time.Now()

	go func() {
		qbuff, err := json.Marshal(q)
		if err != nil {
			panic(err)
		}

		e.in.Write(qbuff)
		e.in.Write([]byte{'\n'})
	}()

	ran := make(chan struct{})

	var buff []byte

	go func() {
		scan := bufio.NewScanner(e.out)
		scan.Scan()

		buff = scan.Bytes()

		close(ran)
	}()

	var result queryResult

	select {
	case <-ctx.Done():
		e.proc.Kill()
		<-ran

		result = queryResult{
			Error: "execution timed out after 1 second",
		}
	case <-ran:
		if err := json.Unmarshal(buff, &result); err != nil {
			result.Error = fmt.Sprintf(
				"evaler send a bad response:\n%v\ngot: %#v",
				err,
				string(buff),
			)
			break
		}
	}

	result.Warm = !fresh
	result.WallTime = uint64(time.Since(startAt))

	err = logTransac(dbname, &transac{
		Query:  *q,
		Result: result,
	})
	if err != nil {
		panic(err)
	}

	remarsh, err := json.Marshal(&result)
	if err != nil {
		panic(err)
	}

	w.Header().Set("Content-Type", "application/json")
	w.Write(remarsh)
}

func alnum(str string) bool {
	for _, c := range str {
		if (c < 'a' || c > 'z') &&
			(c < 'A' || c > 'Z') &&
			(c < '0' || c > '9') {
			return false
		}
	}

	return true
}

func create(w http.ResponseWriter, r *http.Request) {
	err := r.ParseForm()
	if err != nil {
		w.WriteHeader(http.StatusBadRequest)
		w.Write([]byte(err.Error()))
		return
	}

	name := r.FormValue("name")
	lang := r.FormValue("lang")

	if hasDB(name) {
		http.Redirect(w, r, "/query/#"+name, http.StatusFound)
		return
	}

	if len(name) < 2 {
		w.WriteHeader(http.StatusBadRequest)
		w.Write([]byte("name must be at least 2 characters"))
		return
	}

	if !alnum(name) {
		w.WriteHeader(http.StatusBadRequest)
		w.Write([]byte("name must be alphanumeric"))
		return
	}

	if lang == "" {
		http.Redirect(w, r, "/create/#"+name, http.StatusFound)
		return
	}

	if lang != "luaval" && lang != "duktape" {
		w.WriteHeader(http.StatusBadRequest)
		w.Write([]byte("invalid language"))
		return
	}

	err = createDB(name, lang)
	if err != nil {
		if err == errDBExists {
			http.Redirect(w, r, "/query/#"+name, http.StatusFound)
			return
		}

		w.WriteHeader(http.StatusInternalServerError)
		return
	}

	http.Redirect(w, r, "/query/#"+name, http.StatusFound)
}

func main() {
	openDB()

	http.HandleFunc("/eval/", eval)
	http.HandleFunc("/tail/", tail)

	http.HandleFunc("/create", create)

	http.HandleFunc("/memgraph.svg", memgraph)

	// '/', '/query/', and '/create/'
	http.Handle("/", http.FileServer(http.Dir("./client")))

	fmt.Println(strings.Repeat("=", 50))
	log.Println("http on :5000")
	log.Fatalln(http.ListenAndServe(":5000", nil))
}
