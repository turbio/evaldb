package main

import (
	"bufio"
	"context"
	"encoding/json"
	"flag"
	"fmt"
	"html/template"
	"io"
	"io/ioutil"
	"log"
	"net/http"
	"os"
	"os/exec"
	"path"
	"strings"
	"sync"
	"time"
)

func init() {
	log.SetFlags(log.LstdFlags | log.Lshortfile)
}

type query struct {
	Code     string                 `json:"code"`
	Args     map[string]interface{} `json:"args"`
	Gen      *int                   `json:"gen,omitempty"`
	Readonly bool                   `json:"readonly,omitempty"`
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

var dbpath string

func newEvaler(db string) (*evaler, error) {
	evalerName, err := dbEvaler(db)
	if err != nil {
		return nil, err
	}

	cmd := exec.Command("./"+evalerName, "-d", path.Join(dbpath, db), "-s")
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
			http.Error(w, err.Error(), http.StatusBadRequest)
			return nil, false
		}

		var q query
		err = json.Unmarshal(buff, &q)
		if err != nil {
			http.Error(w, err.Error(), http.StatusBadRequest)
			return nil, false
		}

		if q.Args == nil {
			q.Args = map[string]interface{}{}
		}

		return &q, true
	}

	http.Error(w, "json only", http.StatusBadRequest)

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

	if db == "" || !validName(db) {
		return
	}

	grapher := exec.Command("./memgraph", "-d", path.Join(dbpath, db))

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

	ch := make(chan *transac)

	go func() {
		err := tailDB(target, ch)
		if err != nil {
			http.Error(w, "oh no", http.StatusBadRequest)
		}
	}()

	defer unTailDB(ch)

	ctx := r.Context()

	for {
		select {
		case <-ctx.Done():
			return
		case t := <-ch:
			data, _ := json.Marshal(t)
			_, err := w.Write([]byte(
				"event: transac\ndata: " + string(data) + "\n\n",
			))
			if err != nil {
				return
			}

			f.Flush()
		}
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

	dbname := strings.TrimPrefix(r.URL.Path, "/eval/")
	if !hasDB(dbname) {
		http.Error(w, "doesn't exist", http.StatusBadRequest)
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
			Error: "execution timed out after 5 seconds",
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

func validName(str string) bool {
	if len(str) < 2 {
		return false
	}

	if len(str) > 256 {
		return false
	}

	for i, c := range str {
		if c == '_' && i != 0 && i != len(str)-1 {
			continue
		}

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
		http.Error(w, err.Error(), http.StatusBadRequest)
		return
	}

	name := r.FormValue("name")
	lang := r.FormValue("lang")

	if hasDB(name) {
		http.Redirect(w, r, "/query/"+name, http.StatusFound)
		return
	}

	if len(name) < 2 {
		http.Error(
			w,
			"name must be at least 2 characters",
			http.StatusBadRequest,
		)
		return
	}

	if !validName(name) {
		http.Error(
			w,
			"name must be alphanumeric with underscores",
			http.StatusBadRequest,
		)
		return
	}

	if lang == "" {
		http.Redirect(w, r, "/create/"+name, http.StatusFound)
		return
	}

	if lang != "luaval" && lang != "duktape" {
		http.Error(w, "invalid language", http.StatusBadRequest)
		return
	}

	err = createDB(name, lang)
	if err != nil {
		if err == errDBExists {
			http.Redirect(w, r, "/query/"+name, http.StatusFound)
			return
		}

		http.Error(w, "oh no", http.StatusInternalServerError)
		return
	}

	http.Redirect(w, r, "/query/"+name, http.StatusFound)
}

var queryTmpl = template.Must(template.ParseFiles("./client/query.html"))

func queryPage(w http.ResponseWriter, r *http.Request) {
	name := path.Base(r.URL.Path)

	if !hasDB(name) {
		http.NotFound(w, r)
		return
	}

	lang, err := dbEvaler(name)
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}

	err = queryTmpl.Execute(w, struct {
		Name string
		Lang string
	}{
		Name: name,
		Lang: lang,
	})
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
}

func main() {
	dp := flag.String("path", "./db/", "path to the root folder")
	port := flag.String("port", "5000", "port to listen on")

	flag.Parse()

	openDB(path.Join(*dp, "__meta"))
	dbpath = *dp

	http.HandleFunc("/create", create)

	http.HandleFunc("/memgraph.svg", memgraph)

	http.HandleFunc("/eval/", eval)
	http.HandleFunc("/tail/", tail)
	http.HandleFunc("/query/", queryPage)

	http.Handle("/", http.FileServer(http.Dir("./client")))

	fmt.Println(strings.Repeat("=", 50))
	log.Println("http on :" + *port)
	log.Fatalln(http.ListenAndServe(":"+*port, nil))
}
