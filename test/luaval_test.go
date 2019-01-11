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

func TestLuavalMarshal(t *testing.T) {
	db, err := ioutil.TempFile("", "luaval_marshal")
	assert.NoError(t, err)
	defer os.Remove(db.Name())

	cmd := exec.Command(
		"./luaval",
		"-d", db.Name(),
		"-e", "return arg",
		"-a", "arg={\"array\": [1\\,2\\,3]\\, \"float\": 123456.123456\\, \"int\": 1234567890\\, \"t\": true\\, \"f\": false\\, \"str\": \"i'm a string\"}",
	)
	cmd.Dir = "../"

	out, err := cmd.Output()
	assert.NoError(t, err)

	assert.Contains(t, string(out), "\"array\": [1, 2, 3]")
	assert.Contains(t, string(out), "\"float\": 123456.123456")
	assert.Contains(t, string(out), "\"int\": 1234567890")
	assert.Contains(t, string(out), "\"t\": true")
	assert.Contains(t, string(out), "\"f\": false")
	assert.Contains(t, string(out), "\"str\": \"i'm a string\"")
}

func TestLuavalCheckout(t *testing.T) {
	db, err := ioutil.TempFile("", "luaval_checkout")
	assert.NoError(t, err)
	defer os.Remove(db.Name())

	cmd := exec.Command("./luaval", "-d", db.Name(), "-e", "v = 54321")
	cmd.Dir = "../"
	out, err := cmd.Output()
	assert.NoError(t, err)
	assert.Equal(t, "null\n", string(out))

	cmd = exec.Command("./luaval", "-d", db.Name(), "-e", "return v")
	cmd.Dir = "../"
	out, err = cmd.Output()
	assert.NoError(t, err)
	assert.Equal(t, "54321\n", string(out))

	cmd = exec.Command("./luaval", "-d", db.Name(), "-c", "0")
	cmd.Dir = "../"
	_, err = cmd.Output()
	assert.NoError(t, err)

	cmd = exec.Command("./luaval", "-d", db.Name(), "-e", "return v")
	cmd.Dir = "../"
	out, err = cmd.Output()
	assert.NoError(t, err)
	assert.Equal(t, "null\n", string(out))
}

func TestLuaTableCheckout(t *testing.T) {
	db, err := ioutil.TempFile("", "luaval_table_checkout")
	assert.NoError(t, err)
	defer os.Remove(db.Name())

	cmd := exec.Command("./luaval", "-d", db.Name())
	cmd.Dir = "../"
	_, err = cmd.Output()
	assert.NoError(t, err)

	cmd = exec.Command("./luaval", "-d", db.Name(), "-e", "x = {}")
	cmd.Dir = "../"
	out, err := cmd.Output()
	assert.NoError(t, err)
	assert.Equal(t, "null\n", string(out))

	cmd = exec.Command("./luaval", "-d", db.Name(), "-e", "x['first'] = 1")
	cmd.Dir = "../"
	out, err = cmd.Output()
	assert.NoError(t, err)
	assert.Equal(t, "null\n", string(out))

	cmd = exec.Command("./luaval", "-d", db.Name(), "-e", "x['second'] = 2")
	cmd.Dir = "../"
	out, err = cmd.Output()
	assert.NoError(t, err)
	assert.Equal(t, "null\n", string(out))

	cmd = exec.Command("./luaval", "-d", db.Name(), "-e", "x['third'] = 3")
	cmd.Dir = "../"
	out, err = cmd.Output()
	assert.NoError(t, err)
	assert.Equal(t, "null\n", string(out))

	cmd = exec.Command("./luaval", "-d", db.Name(), "-e", "return x")
	cmd.Dir = "../"
	out, err = cmd.Output()
	assert.NoError(t, err)
	assert.Contains(t, string(out), "\"first\": 1")
	assert.Contains(t, string(out), "\"second\": 2")
	assert.Contains(t, string(out), "\"third\": 3")

	cmd = exec.Command("./luaval", "-d", db.Name(), "-c", "4")
	cmd.Dir = "../"
	_, err = cmd.Output()
	assert.NoError(t, err)

	cmd = exec.Command("./luaval", "-d", db.Name(), "-e", "return x")
	cmd.Dir = "../"
	out, err = cmd.Output()
	assert.NoError(t, err)
	assert.Contains(t, string(out), "\"first\": 1")
	assert.Contains(t, string(out), "\"second\": 2")
	assert.NotContains(t, string(out), "\"third\": 3")
}
