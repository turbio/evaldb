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
	"net/http"
	"os"
	"os/exec"
	"path"
	"strings"
	"sync"
	"time"

	"github.com/google/uuid"
	log "github.com/sirupsen/logrus"
)

type query struct {
	Code     string                 `json:"code"`
	Args     map[string]interface{} `json:"args"`
	Gen      *int                   `json:"gen,omitempty"`
	Readonly bool                   `json:"readonly,omitempty"`
}

func (q *query) String() string {
	return fmt.Sprintf("%#v (%#v)", q.Code, q.Args)
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
	db string

	in  io.Writer
	out io.Reader

	cmd *exec.Cmd

	sync.Mutex
}

var evalers = []*evaler{}
var elock = sync.RWMutex{}

var userDBsPath string

func newEvaler(db string) (*evaler, error) {
	evalerName, err := dbEvaler(db)
	if err != nil {
		return nil, err
	}

	cmd := exec.Command("./"+evalerName, "-d", path.Join(userDBsPath, db), "-s")
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
		cmd:   cmd,
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

func (e *evaler) timeout() {
	e.cmd.Process.Kill()
	e.cmd.Wait()

	elock.Lock()
	defer elock.Unlock()
	for i, ev := range evalers {
		if ev.db == e.db {
			evalers[i] = evalers[len(evalers)-1]
			evalers = evalers[:len(evalers)-1]
			return
		}
	}

	panic("oh nos")
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

	if db == "" {
		return
	}

	grapher := exec.Command("./memgraph", "-d", path.Join(userDBsPath, db))

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
		log.WithField("db", db).WithError(err).Println("cant start grapher")
		w.WriteHeader(500)
		return
	}

	err = renderer.Start()
	if err != nil {
		log.WithField("db", db).WithError(err).Println("cant start render")
		w.WriteHeader(500)
		return
	}

	err = grapher.Wait()
	if err != nil {
		log.WithField("db", db).WithError(err).Println("cant wait grapher")
		w.WriteHeader(500)
		return
	}

	err = renderer.Wait()
	if err != nil {
		log.WithField("db", db).WithError(err).Println("cant wait render")
		w.WriteHeader(500)
		return
	}
}

func tail(w http.ResponseWriter, r *http.Request) {
	target := strings.TrimPrefix(r.URL.Path, "/tail/")
	log.WithField("db", target).Println("tailing")

	w.Header().Set("Content-Type", "text/event-stream")
	f := w.(http.Flusher)

	ch := make(chan *transac)

	go func() {
		err := tailDB(target, ch)
		if err != nil {
			log.WithField("db", target).WithError(err).Println("tail error")
			http.Error(w, "oh no", http.StatusBadRequest)
		}
	}()

	defer unTailDB(ch)

	ctx := r.Context()

	for {
		select {
		case <-ctx.Done():
			log.WithField("db", target).Println("tail done")
			return
		case t := <-ch:
			data, _ := json.Marshal(t)
			_, err := w.Write([]byte(
				"event: transac\ndata: " + string(data) + "\n\n",
			))
			if err != nil {
				log.WithField("db", db).WithError(err).Println("tail write error")
				return
			}

			f.Flush()
		}
	}
}

func doEval(dbname string, q *query) *queryResult {
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	defer cancel()

	e, fresh, err := acquireEvaler(dbname)
	if err != nil {
		log.WithField("db", dbname).WithError(err).Println("unable to acquire")
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
		log.WithField("db", dbname).
			WithField("query", q).
			WithError(err).Println("execution timed out")

		e.timeout()
		<-ran

		result = queryResult{
			Parent: 0,
			Gen:    -1,
			Error:  "execution timed out",
		}
	case <-ran:
		if err := json.Unmarshal(buff, &result); err != nil {
			result.Parent = 0
			result.Gen = -1

			log.WithField("db", dbname).
				WithField("query", q).
				WithError(err).
				Printf("unable to unmarshal: %#v", string(buff))

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

	return &result
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

	log.WithField("db", dbname).WithField("query", q).Println("evaling")

	result := doEval(dbname, q)

	remarsh, err := json.Marshal(result)
	if err != nil {
		panic(err)
	}

	w.Header().Set("Content-Type", "application/json")
	w.Write(remarsh)
}

func create(w http.ResponseWriter, r *http.Request) {
	err := r.ParseForm()
	if err != nil {
		log.WithError(err).Println("unable to parse create")
		http.Error(w, err.Error(), http.StatusBadRequest)
		return
	}

	lang := r.FormValue("lang")

	name := uuid.New().String()

	if hasDB(name) {
		panic("wut?")
	}

	if lang == "" {
		http.Error(w, "not sure about that language", http.StatusBadRequest)
		return
	}

	if lang != "luaval" && lang != "duktape" {
		log.WithError(err).Println("unepxected language:", lang)
		http.Error(w, "invalid language", http.StatusBadRequest)
		return
	}

	err = createDB(name, lang)
	if err != nil {
		if err == errDBExists {
			log.WithField("db", name).Println("db exists, weird")
			http.Redirect(w, r, "/query/"+name, http.StatusFound)
			return
		}

		log.WithField("db", name).WithError(err).Println("unable to create db")

		http.Error(w, "oh no", http.StatusInternalServerError)
		return
	}

	log.WithField("db", name).WithField("lang", lang).Println("created db")

	http.Redirect(w, r, "/query/"+name, http.StatusFound)
}

func link(w http.ResponseWriter, r *http.Request) {
	err := r.ParseForm()
	if err != nil {
		http.Error(w, "unable to parse link", http.StatusBadRequest)
		return
	}

	dbname := r.FormValue("dbname")
	if !hasDB(dbname) {
		http.Error(w, "huh that db doesn't exist???", http.StatusBadRequest)
		return
	}

	hostname := r.FormValue("hostname")
	if existing, _ := dbForLink(hostname); existing != "" {
		http.Error(w, "another db is using that hostname", http.StatusBadRequest)
		return
	}

	if err := setLink(dbname, hostname); err != nil {
		panic(err)
	}

	log.WithField("db", dbname).WithField("host", hostname).Println("linked domain")

	http.Redirect(w, r, "/query/"+dbname, http.StatusFound)
}

func queryPage(w http.ResponseWriter, r *http.Request) {
	dbname := strings.TrimPrefix(r.URL.Path, "/query/")
	log.WithField("db", dbname).Println("get query page")

	if !hasDB(dbname) {
		http.Error(w, "not found", http.StatusNotFound)
		return
	}

	lang, err := dbEvaler(dbname)
	if err != nil {
		http.Error(w, "not found", http.StatusInternalServerError)
		return
	}

	link, err := linkForDB(dbname)
	if err != nil {
		link = ""
	}

	var indexTmpl = template.Must(template.ParseFiles("./client/query.html"))
	err = indexTmpl.Execute(w, struct {
		Lang string
		Name string
		Link string
	}{
		Lang: lang,
		Name: dbname,
		Link: link,
	})
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
}

func webReqHandle(w http.ResponseWriter, r *http.Request) {
	dbname, err := dbForHost(r.Host)

	if err == errDBNotExists {
		w.WriteHeader(http.StatusNotFound)
		w.Write([]byte("404"))
		log.WithField("db", dbname).
			WithField("path", r.URL.Path).
			WithField("host", r.Host).
			Println("web req not found")
		return
	} else if err != nil {
		panic(err)
	}

	log.WithField("db", dbname).
		WithField("path", r.URL.Path).
		WithField("host", r.Host).
		Println("web req")

	result := doEval(dbname, &query{
		Code: "return http.req(r)",
		Args: map[string]interface{}{
			"r": map[string]interface{}{
				"path":   r.URL.Path,
				"method": r.Method,
				//"headers": r.Header,
			},
		},
	})

	if result.Error != nil {
		w.WriteHeader(http.StatusInternalServerError)

		if asStr, ok := result.Error.(string); ok {
			w.Write([]byte(asStr))
		} else {
			w.Write([]byte("internal error, route return object not a string"))
		}
		return
	}

	if asStr, ok := result.Object.(string); ok {
		w.Write([]byte(asStr))
	} else if asObj, ok := result.Object.(map[string]interface{}); ok {
		status := http.StatusOK
		if maybeKey, ok := asObj["status"]; ok {
			if maybeInt, ok := maybeKey.(float64); ok {
				status = int(maybeInt)
			}
		}

		if maybeKey, ok := asObj["headers"]; ok {
			if maybeHeaders, ok := maybeKey.(map[string]interface{}); ok {
				for key, val := range maybeHeaders {
					if maybeStr, ok := val.(string); ok {
						w.Header().Set(key, maybeStr)
					}
				}
			}
		}

		w.WriteHeader(status)

		if maybeKey, ok := asObj["body"]; ok {
			if maybeBody, ok := maybeKey.(string); ok {
				w.Write([]byte(maybeBody))
			}
		}
	} else {
		w.WriteHeader(http.StatusInternalServerError)
		w.Write([]byte("internal error, route didn't return a string"))
	}
}

func main() {
	dp := flag.String("path", "./db/", "path to the root folder")
	port := flag.String("port", "5000", "port to listen on")

	flag.Parse()

	openDB(path.Join(*dp, "__meta"))
	userDBsPath = path.Join(*dp, "dbs")

	dbMux := &http.ServeMux{}
	dbMux.HandleFunc("/create", create)
	dbMux.HandleFunc("/link", link)
	dbMux.HandleFunc("/memgraph.svg", memgraph)
	dbMux.HandleFunc("/eval/", eval)
	dbMux.HandleFunc("/tail/", tail)
	dbMux.HandleFunc("/query/", queryPage)
	dbMux.Handle("/", http.FileServer(http.Dir("./client")))

	webMux := &http.ServeMux{}
	webMux.HandleFunc("/", webReqHandle)

	root := &http.ServeMux{}
	root.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		if isRoot(r.Host) {
			dbMux.ServeHTTP(w, r)
		} else {
			webMux.ServeHTTP(w, r)
		}
	})

	log.Println("http on :" + *port)
	log.Fatalln(http.ListenAndServe(":"+*port, root))
}

func dbForHost(host string) (string, error) {
	host = strings.TrimSuffix(host, ".localhost:5000")
	host = strings.TrimSuffix(host, ".turb.io")
	return dbForLink(host)
}

func isRoot(host string) bool {
	return host == "localhost:5000" || host == "evaldb.turb.io"
}
