package main

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io/ioutil"
	"net/http"
	"net/url"
	"os"
	"os/exec"
	"strings"
	"testing"
	"time"

	assert "github.com/stretchr/testify/require"
)

func TestMain(m *testing.M) {
	cmd := exec.Command("./gateway")
	cmd.Dir = "../"
	cmd.Stderr = os.Stderr
	cmd.Stdout = os.Stdout

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

func makeDB(lang string) string {
	c := &http.Client{
		CheckRedirect: func(req *http.Request, via []*http.Request) error {
			return http.ErrUseLastResponse
		},
	}

	res, err := c.PostForm("http://localhost:5000/create", url.Values{
		"lang": {lang},
	})
	if err != nil {
		panic(err)
	}

	if res.StatusCode != 302 {
		s, _ := ioutil.ReadAll(res.Body)
		fmt.Println(string(s))
		panic(res.Status)
	}

	return strings.TrimPrefix(res.Header.Get("Location"), "/query/")
}

func makeQuery(q *query, db string) (*queryResult, error) {

	qmarsh, _ := json.Marshal(q)

	res, err := http.Post(
		"http://localhost:5000/eval/"+db,
		"application/json",
		bytes.NewReader(qmarsh),
	)
	if err != nil {
		return nil, err
	}

	if res.StatusCode != http.StatusOK {
		return nil, errors.New(res.Status)
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

func TestSomeErrors(t *testing.T) {
	db := makeDB("duktape")

	result1, err := makeQuery(&query{Code: "asdf"}, db)
	assert.NoError(t, err)
	assert.NotEmpty(t, result1.Error)

	result2, err := makeQuery(&query{Code: "asdf"}, db)
	assert.NoError(t, err)
	assert.NotEmpty(t, result2.Error)
	assert.Equal(t, result1.Error, result2.Error)
}

func TestSuperSimpleExpression(t *testing.T) {
	db := makeDB("luaval")

	result, err := makeQuery(&query{Code: "return 1+1"}, db)
	assert.NoError(t, err)

	assert.EqualValues(t, 2, result.Object)
	assert.Empty(t, result.Error)
}

func TestCounting(t *testing.T) {
	db := makeDB("luaval")

	result, err := makeQuery(&query{Code: "counter = 0"}, db)
	assert.NoError(t, err)
	assert.Empty(t, result.Error)
	assert.Nil(t, result.Object)

	for i := 1; i < 100; i++ {
		result, err := makeQuery(&query{Code: "counter = counter + 1\nreturn counter"}, db)
		assert.NoError(t, err)
		assert.Empty(t, result.Error)
		assert.EqualValues(t, i, result.Object)
	}
}

func BenchmarkCountLua(b *testing.B) {
	db := makeDB("luaval")

	makeQuery(&query{Code: "counter = 0"}, db)

	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		makeQuery(&query{Code: "counter = counter + 1\nreturn counter"}, db)
	}
}

func BenchmarkCountDuktape(b *testing.B) {
	db := makeDB("duktape")

	makeQuery(&query{Code: "counter = 0"}, db)

	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		makeQuery(&query{Code: "counter = counter + 1\nreturn counter"}, db)
	}
}
