package main

import (
	"log"
	"net/http"
)

func main() {
	http.Handle("/", http.FileServer(http.Dir("./client")))

	log.Println("http on :5000")
	log.Fatalln(http.ListenAndServe(":5000", nil))
}
