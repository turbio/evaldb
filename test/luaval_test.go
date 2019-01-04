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

func TestLuavalAddArgs(t *testing.T) {
	db, err := ioutil.TempFile("", "luaval_add_args")
	assert.NoError(t, err)
	defer os.Remove(db.Name())

	cmd := exec.Command(
		"./luaval",
		"-d", db.Name(),
		"-e", "return first + second",
		"-a", "first=100",
		"-a", "second=200",
	)
	cmd.Dir = "../"

	out, err := cmd.Output()
	assert.NoError(t, err)
	assert.Equal(t, "300\n", string(out))
}

func TestLuavalTable(t *testing.T) {
	db, err := ioutil.TempFile("", "luaval_table")
	assert.NoError(t, err)
	defer os.Remove(db.Name())

	cmd := exec.Command(
		"./luaval",
		"-d", db.Name(),
		"-e", "return first + second",
		"-a", "first=100",
		"-a", "second=200",
	)
	cmd.Dir = "../"

	out, err := cmd.Output()
	assert.NoError(t, err)
	assert.Equal(t, "300\n", string(out))
}
