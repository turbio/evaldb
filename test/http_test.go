package main

import (
	"encoding/json"
	"fmt"
	"io/ioutil"
	"net/http"
	"net/url"
	"os"
	"os/exec"
	"testing"
	"time"

	"github.com/stretchr/testify/assert"
)

func TestMain(m *testing.M) {
	cmd := exec.Command("./evalserver")
	cmd.Dir = "../"
	//cmd.Stderr = os.Stderr
	//cmd.Stdout = os.Stdout

	err := cmd.Start()
	if err != nil {
		panic(err)
	}

	tries := 0

	wait := make(chan error)

	go func() {
		wait <- cmd.Wait()
	}()

	fmt.Println("waiting for server to come up...")
	for {
		_, err = http.Get("http://localhost:5000")

		if err == nil {
			break
		}

		tries++

		if tries > 100 {
			panic("unable to connect after many tries")
		}

		select {
		case <-wait:
			panic("process exited early")
		default:
		}

		fmt.Println(".")
		time.Sleep(time.Millisecond * 100)
	}

	status := m.Run()

	select {
	case <-wait:
		panic("process exited early")
	default:
	}

	err = cmd.Process.Kill()
	if err != nil {
		panic(err)
	}

	os.Exit(status)
}

type queryResult struct {
	Object   interface{} `json:"object"`
	Error    string      `json:"error"`
	Warm     bool        `json:"warm"`
	WallTime uint64      `json:"walltime"`
	Seq      string      `json:"seq"`
}

type query struct {
	Code string `json:"code"`
	Seq  string `json:"seq"`
}

func makeQuery(q *query) (*queryResult, error) {
	res, err := http.PostForm(
		"http://localhost:5000/eval",
		url.Values{
			"code": {q.Code},
			"seq":  {q.Seq},
		},
	)
	if err != nil {
		return nil, err
	}

	body, err := ioutil.ReadAll(res.Body)
	if err != nil {
		return nil, err
	}

	result := queryResult{}
	err = json.Unmarshal(body, &result)
	if err != nil {
		return nil, err
	}

	return &result, nil
}

func TestSuperSimpleExpression(t *testing.T) {
	result, err := makeQuery(&query{Code: "return 1+1"})
	assert.NoError(t, err)

	assert.EqualValues(t, 2, result.Object)
	assert.Empty(t, result.Error)
}

func TestCounting(t *testing.T) {
	result, err := makeQuery(&query{Code: "counter = 0"})
	assert.NoError(t, err)
	assert.Empty(t, result.Error)
	assert.Nil(t, result.Object)

	for i := 1; i < 100; i++ {
		result, err := makeQuery(&query{Code: "counter = counter + 1\nreturn counter"})
		assert.NoError(t, err)
		assert.Empty(t, result.Error)
		assert.EqualValues(t, i, result.Object)
	}
}

func BenchmarkCount(b *testing.B) {
	makeQuery(&query{Code: "counter = 0"})

	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		makeQuery(&query{Code: "counter = counter + 1\nreturn counter"})
	}
}
