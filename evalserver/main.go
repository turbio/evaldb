package main

import (
	"context"
	"log"
	"net/http"
	"os"
	"os/exec"
	"time"
)

func eval(w http.ResponseWriter, r *http.Request) {
	ctx, cancel := context.WithTimeout(r.Context(), 3*time.Second)
	defer cancel()

	err := r.ParseForm()
	if err != nil {
		log.Println("error:", err)
		w.WriteHeader(http.StatusBadRequest)
		return
	}

	cmd := exec.CommandContext(
		ctx,
		"./luaval",
		"__db1",
		r.FormValue("query"),
	)
	cmd.Stdout = w
	cmd.Stderr = os.Stderr

	err = cmd.Run()
	if err != nil {
		log.Println("error:", err)
		w.WriteHeader(http.StatusBadRequest)
	}
}

func main() {
	http.HandleFunc("/eval", eval)
	http.Handle("/", http.FileServer(http.Dir("./client")))

	log.Println("http on :5000")
	log.Fatalln(http.ListenAndServe(":5000", nil))
}
