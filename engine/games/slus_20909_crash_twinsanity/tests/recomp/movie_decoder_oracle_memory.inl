namespace movie_oracle::memory {
template<class T> T read(uint32_t address) {
    T value;
    Read(address, &value, sizeof(value));
    return value;
}
}  // namespace movie_oracle::memory
