package main

import (
	"io/ioutil"
	"os"
	"os/exec"
	"testing"

	"github.com/stretchr/testify/assert"
)

func TestCountTo1000(t *testing.T) {
	db, err := ioutil.TempFile("", "testcounter_count_to_1000")
	assert.NoError(t, err)
	os.Remove(db.Name())

	var out []byte
	for i := 0; i < 1000; i++ {
		cmd := exec.Command("./testcounter", "-d", db.Name(), "-e", "")
		cmd.Dir = "../"
		out, err = cmd.Output()
		if err != nil {
			break
		}
	}

	assert.NoError(t, err)
	assert.Equal(t, "1000\n", string(out))
}
