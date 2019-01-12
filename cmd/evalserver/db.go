package main

import (
	"encoding/json"
	"errors"
	"log"
	"strconv"

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
	return db.Update(func(tx *bolt.Tx) error {
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
}

func openDB() {
	var err error
	db, err = bolt.Open("./db/__global", 0666, nil)
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
			log.Printf("db: %s: with %d keys", k, d.Stats().KeyN)

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
