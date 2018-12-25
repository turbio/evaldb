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

const defaultDB = "default"

func init() {
	log.SetFlags(log.LstdFlags | log.Lshortfile)
}

type query struct {
	Code string                 `json:"code"`
	Args map[string]interface{} `json:"args"`
	Seq  string                 `json:"seq"`
}

type queryResult struct {
	Object   interface{} `json:"object"`
	Error    interface{} `json:"error,omitempty"`
	Warm     bool        `json:"warm"`
	WallTime uint64      `json:"walltime"`
	Seq      string      `json:"seq"`
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
	cmd := exec.Command("./luaval", "-d", "./db/__"+db, "-s")
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

	if err := r.ParseForm(); err != nil {
		w.WriteHeader(http.StatusBadRequest)
		w.Write([]byte(err.Error()))
		return nil, false
	}

	q := &query{
		Code: r.FormValue("code"),
		Seq:  r.FormValue("seq"),
		Args: map[string]interface{}{},
	}

	return q, true
}

func memgraph(w http.ResponseWriter, r *http.Request) {

	renderer := exec.Command("dot", "-Tsvg")
	grapher := exec.Command("./memgraph", "-d", "./db/__"+defaultDB)

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

func eval(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Access-Control-Allow-Origin", "*")
	w.Header().Set(
		"Access-Control-Allow-Methods",
		"POST, GET, OPTIONS, PUT, DELETE",
	)

	if r.Method == "HEAD" || r.Method == "OPTIONS" {
		return
	}

	r.Body = http.MaxBytesReader(w, r.Body, MaxRequestSize)

	q, ok := parseQuery(w, r)
	if !ok {
		return
	}

	ctx, cancel := context.WithTimeout(r.Context(), 1*time.Second)
	defer cancel()

	e, fresh, err := acquireEvaler(defaultDB)
	if err != nil {
		panic(err)
	}

	defer e.release()

	seq, err := logQuery(defaultDB, q)
	if err != nil {
		panic(err)
	}

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
	result.Seq = seq

	logResult(defaultDB, &result)

	remarsh, err := json.Marshal(&result)
	if err != nil {
		panic(err)
	}

	w.Header().Set("Content-Type", "application/json")
	w.Write(remarsh)
}

func main() {
	openDB()
	err := createDB(defaultDB)
	if err != nil {
		log.Println("info:", err)
	}

	http.HandleFunc("/eval", eval)
	http.HandleFunc("/memgraph.svg", memgraph)
	http.Handle("/", http.FileServer(http.Dir("./client")))

	fmt.Println(strings.Repeat("=", 50))
	log.Println("http on :5000")
	log.Fatalln(http.ListenAndServe(":5000", nil))
}
