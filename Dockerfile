FROM golang:1.11

RUN apt-get update
RUN apt-get install -y libjansson-dev
RUN apt-get install -y libreadline-dev
RUN apt-get install -y graphviz
RUN apt-get install -y clang

RUN mkdir -p /go/src/github.com/turbio/evaldb

WORKDIR /go/src/github.com/turbio/evaldb

COPY . .

RUN go get ./cmd/gateway
RUN make

RUN mkdir /db

EXPOSE 5000

CMD ./gateway -path /db
