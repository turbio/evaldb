package main

import (
	"encoding/json"
	"errors"
	"log"
	"strconv"
	"sync"

	"github.com/boltdb/bolt"
)

var errDBExists = errors.New("db already exists")
var errDBNotExists = errors.New("db does not exist")

var dbsBucket = []byte("dbs")

var db *bolt.DB

func hasDB(dbid string) bool {
	found := false

	db.View(func(tx *bolt.Tx) error {
		rdbs := tx.Bucket(dbsBucket)
		found = rdbs.Bucket([]byte(dbid)) != nil
		return nil
	})

	return found
}

var logsLock sync.RWMutex
var logcbs = map[string][]chan *transac{}

func tailDB(dbid string, s chan *transac) error {
	err := db.View(func(tx *bolt.Tx) error {
		rdbs := tx.Bucket(dbsBucket)
		dbb := rdbs.Bucket([]byte(dbid))
		if dbb == nil {
			return errors.New("internal error")
		}

		logs := dbb.Bucket([]byte("logs"))
		if logs == nil {
			return errors.New("internal error")
		}

		c := logs.Cursor()
		for k, v := c.First(); k != nil; k, v = c.Next() {
			var t transac
			if json.Unmarshal(v, &t) == nil {
				s <- &t
			}
		}

		return nil
	})
	if err != nil {
		return err
	}

	logsLock.Lock()
	cbs, ok := logcbs[dbid]
	if !ok {
		cbs = []chan *transac{}
	}
	logcbs[dbid] = append(cbs, s)
	logsLock.Unlock()

	return nil
}

func unTailDB(ch chan *transac) {
	logsLock.Lock()
	defer logsLock.Unlock()

	for dbid, chs := range logcbs {
		for j, dbch := range chs {
			if dbch == ch {
				logcbs[dbid] = append(chs[:j], chs[j+1:]...)
				return
			}
		}
	}
}

func createDB(dbid, lang string) error {
	return db.Update(func(tx *bolt.Tx) error {
		rdbs := tx.Bucket(dbsBucket)
		exists := rdbs.Bucket([]byte(dbid)) != nil
		if exists {
			return errDBExists
		}

		newDB, err := rdbs.CreateBucket([]byte(dbid))
		if err != nil {
			return err
		}

		err = newDB.Put([]byte("lang"), []byte(lang))
		if err != nil {
			return err
		}

		_, err = newDB.CreateBucket([]byte("logs"))
		return err
	})
}

func logTransac(dbid string, t *transac) error {
	err := db.Update(func(tx *bolt.Tx) error {
		dbb := tx.Bucket(dbsBucket).Bucket([]byte(dbid))
		if dbb == nil {
			return errDBNotExists
		}

		logs := dbb.Bucket([]byte("logs"))
		if logs == nil {
			return errors.New("internal error")
		}

		j, err := json.Marshal(t)
		if err != nil {
			return err
		}

		s, _ := logs.NextSequence()

		return logs.Put([]byte(strconv.FormatUint(s, 10)+".t"), j)
	})

	if err != nil {
		return err
	}

	logsLock.Lock()
	cbs, ok := logcbs[dbid]
	if ok {
		for _, s := range cbs {
			s <- t
		}
	}
	logsLock.Unlock()

	return nil
}

func openDB(path string) {
	var err error
	db, err = bolt.Open(path, 0666, nil)
	if err != nil {
		log.Fatalln("unable to open global db:", err)
	}

	err = db.Update(func(tx *bolt.Tx) error {
		dbb, errr := tx.CreateBucketIfNotExists(dbsBucket)
		if errr != nil {
			return errr
		}

		dbb.ForEach(func(k []byte, v []byte) error {
			d := dbb.Bucket(k)
			log.Printf("%s:", k)

			d.ForEach(func(k []byte, v []byte) error {
				log.Printf("\t%s: %s", k, v)
				return nil
			})

			return nil
		})

		return nil
	})
	if err != nil {
		log.Fatalln("cannot setup db:", err)
	}
}
