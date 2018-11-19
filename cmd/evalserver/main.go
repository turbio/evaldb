package main

import (
	"bufio"
	"context"
	"encoding/json"
	"fmt"
	"io/ioutil"
	"log"
	"net/http"
	"os"
	"os/exec"
	"strings"
	"time"
)

func init() {
	log.SetFlags(log.LstdFlags | log.Lshortfile)
}

type query struct {
	Code string `json:"code"`
}

type queryResult struct {
	Object interface{} `json:"object"`
	Error  string      `json:"error"`
	Cold   bool        `json:"cold"`
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

		return &q, true
	}

	if err := r.ParseForm(); err != nil {
		w.WriteHeader(http.StatusBadRequest)
		w.Write([]byte(err.Error()))
		return nil, false
	}

	q := &query{Code: r.FormValue("code")}

	return q, true
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

	var buff []byte

	cmd := exec.Command("./luaval", "__db1", "-")
	cmd.Stderr = os.Stderr

	stdin, _ := cmd.StdinPipe()
	stdout, _ := cmd.StdoutPipe()

	qbuff, err := json.Marshal(q)
	if err != nil {
		log.Println(err)
		w.WriteHeader(http.StatusInternalServerError)
		w.Write([]byte(err.Error()))
		return
	}

	go func() {
		stdin.Write(qbuff)
		stdin.Write([]byte{'\n'})
	}()

	ran := make(chan struct{})

	go func() {
		err := cmd.Start()

		if err != nil {
			log.Println("error:", err)
		}

		scan := bufio.NewScanner(stdout)
		scan.Scan()

		buff = scan.Bytes()

		close(ran)
	}()

	select {
	case <-ctx.Done():
		cmd.Process.Kill()
		<-ran

		result, err := json.Marshal(&queryResult{
			Error: "execution timed out after 1 second",
		})
		if err != nil {
			w.WriteHeader(http.StatusInternalServerError)
			w.Write([]byte(err.Error()))
			return
		}

		w.Header().Set("Content-Type", "application/json")
		w.Write(result)

	case <-ran:
		var qr queryResult
		err := json.Unmarshal(buff, &qr)
		if err != nil {
			w.WriteHeader(http.StatusInternalServerError)
			w.Write([]byte(string(buff) + "\n" + err.Error()))
			return
		}

		normalized, err := json.Marshal(&qr)
		if err != nil {
			w.WriteHeader(http.StatusInternalServerError)
			w.Write([]byte(err.Error()))
			return
		}

		w.Header().Set("Content-Type", "application/json")
		w.Write(normalized)
	}
}

func main() {
	http.HandleFunc("/eval", eval)
	http.Handle("/", http.FileServer(http.Dir("./client")))

	fmt.Println(strings.Repeat("=", 50))
	log.Println("http on :5000")
	log.Fatalln(http.ListenAndServe(":5000", nil))
}
