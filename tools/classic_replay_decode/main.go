package main

import (
    "bytes"
    "encoding/binary"
    "errors"
    "fmt"
    "io"
    "os"

    "github.com/icza/screp/repparser/repdecoder"
)

// OpenBW expects a section-uncompressed classic replay. The tournament manager
// saves legacy .rep files; the Remastered-only converter cannot decode those.
func decode(source, destination string) error {
    reader, err := repdecoder.NewFromFile(source)
    if err != nil { return err }
    defer reader.Close()
    parts := make([][]byte, 0, 4)
    for index := 0; index < 4; index++ {
        if err := reader.NewSection(); err != nil { return fmt.Errorf("section %d: %w", index, err) }
        size := int32(4)
        if index == 1 { size = 633 }
        if index == 2 || index == 3 {
            prefix, _, err := reader.Section(4)
            if err != nil { return err }
            length := binary.LittleEndian.Uint32(prefix)
            if length == 0 || length > 32<<20 { return errors.New("invalid replay section size") }
            size = int32(length)
        }
        part, _, err := reader.Section(size)
        if err != nil { return err }
        parts = append(parts, part)
    }
    if !bytes.Equal(parts[0], []byte("reRS")) { return errors.New("not a classic replay") }
    var result bytes.Buffer
    result.Write(parts[0])
    result.Write(parts[1])
    for _, part := range parts[2:] {
        if err := binary.Write(&result, binary.LittleEndian, uint32(len(part))); err != nil { return err }
        result.Write(part)
    }
    output, err := os.OpenFile(destination, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0600)
    if err != nil { return err }
    defer output.Close()
    _, err = io.Copy(output, &result)
    return err
}

func main() {
    if len(os.Args) != 3 {
        fmt.Fprintln(os.Stderr, "usage: classic_replay_decode INPUT.rep OUTPUT.raw")
        os.Exit(2)
    }
    if err := decode(os.Args[1], os.Args[2]); err != nil {
        fmt.Fprintln(os.Stderr, err)
        os.Exit(1)
    }
}
