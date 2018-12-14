package main

import (
	"io/ioutil"
	"os/exec"
	"testing"
)

func TestSimpleEval(t *testing.T) {
	db, err := ioutil.TempFile("", "luaval_1+1")
	if err != nil {
		panic(err)
	}

	cmd := exec.Command("./luaval", db.Name(), "return 1 + 1")
	cmd.Dir = "../"

	err = cmd.Run()
	if err != nil {
		panic(err)
	}
}
