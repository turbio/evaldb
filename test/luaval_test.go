package main

import (
	"io/ioutil"
	"os"
	"os/exec"
	"testing"

	"github.com/stretchr/testify/assert"
)

func TestLuavalOnePlusOne(t *testing.T) {
	db, err := ioutil.TempFile("", "luaval_one_plus_one")
	assert.NoError(t, err)
	defer os.Remove(db.Name())

	cmd := exec.Command("./luaval", "-d", db.Name(), "-e", "return 1 + 1")
	cmd.Dir = "../"

	out, err := cmd.Output()
	assert.NoError(t, err)
	assert.Equal(t, "2\n", string(out))
}

func TestLuavalSetGet(t *testing.T) {
	db, err := ioutil.TempFile("", "luaval_set_get")
	assert.NoError(t, err)
	defer os.Remove(db.Name())

	cmd := exec.Command("./luaval", "-d", db.Name(), "-e", "v = 12345")
	cmd.Dir = "../"

	out, err := cmd.Output()
	assert.NoError(t, err)
	assert.Equal(t, "null\n", string(out))

	cmd = exec.Command("./luaval", "-d", db.Name(), "-e", "return v")
	cmd.Dir = "../"

	out, err = cmd.Output()
	assert.NoError(t, err)
	assert.Equal(t, "12345\n", string(out))
}
