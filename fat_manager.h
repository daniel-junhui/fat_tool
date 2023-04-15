#pragma once

#include "fat.h"
#include "fat_map.h"
#include "fsinfo_manager.h"
#include <cassert>
#include <cstdint>
#include <fcntl.h>
#include <memory>
#include <optional>
#include <string>
#include <sys/mman.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#define ASSERT(x)       assert(x)
#define ASSERT_EQ(x, y) assert((x) == (y))
#define REM(x, y)       ((x) % (y))

namespace cs5250 {

template <typename T, typename... Args>
bool IsOneOf(const T &input, Args... args) {
    return ((input == args) || ...);
}

template <size_t N> bool AllZero(const uint8_t (&arr)[N]) {
    for (size_t i = 0; i < N; ++i) {
        if (arr[i] != 0) {
            return false;
        }
    }
    return true;
}

template <typename E>
constexpr auto ToIntegral(E e) -> typename std::underlying_type<E>::type {
    return static_cast<typename std::underlying_type<E>::type>(e);
}

template <typename T>
concept StringConvertible = std::convertible_to<T, std::string>;

template <typename T>
using OptionalRef = std::optional<std::reference_wrapper<T>>;

class FATManager {
  private:
    const std::string file_path_;
    uint8_t *image_ = nullptr;
    off_t image_size_ = 0;
    uint32_t root_cluster_number_ = 0;
    std::unique_ptr<FATMap> fat_map_;
    std::unordered_map<SimpleStruct, std::vector<SimpleStruct>> dir_map_;
    SimpleStruct root_dir_;
    std::unique_ptr<FSInfoManager> fs_info_manager_;

    bool IsFreeDirEntry(const FATDirectory *dir) {
        return dir->DIR_Name.name[0] == 0x00;
    }

    bool IsDeletedDirEntry(const FATDirectory *dir) {
        return dir->DIR_Name.name[0] == 0xE5; // deleted
    }

    template <typename F>
    void ForEverySectorOfCluster(uint32_t cluster_number,
                                 F &&function_for_sector) {
        auto first_sector_number =
            FirstSectorNumberOfDataCluster(cluster_number);

        for (int i = 0; i < sectors_per_cluster_; ++i) {
            auto start_address = StartAddressOfSector(first_sector_number + i);
            function_for_sector(start_address);
        }
    }

    template <typename F>
    void ForEverySectorOfFile(const SimpleStruct &dir, F &&function) {

        auto cluster_number = dir.first_cluster;

        do {
            ForEverySectorOfCluster(cluster_number, function);
            cluster_number = fat_map_->Lookup(cluster_number);
        } while (!IsEndOfFile(cluster_number));
    }

    template <typename F>
    void ForEveryClusterOfFile(const SimpleStruct &dir, F &&function) {
        auto cluster_number = dir.first_cluster;

        do {
            function(cluster_number);
            cluster_number = fat_map_->Lookup(cluster_number);
        } while (!IsEndOfFile(cluster_number));
    }

    template <typename Func>
    void ForEveryDirEntryInDirSector(const uint8_t *data, Func &&func) {
        auto dirs_per_sector = bytes_per_sector_ / sizeof(FATDirectory);
        for (decltype(dirs_per_sector) index = 0; index < dirs_per_sector;
             ++index) {
            auto dir = reinterpret_cast<const FATDirectory *>(
                data + index * sizeof(FATDirectory));
            if (IsFreeDirEntry(dir)) {
                // std::cout << "find a free entry\n";
                break;
            } else if (IsDeletedDirEntry(dir)) {
                // std::cout << "find a deleted entry\n";
                continue;
            } else
                func(dir);
        }
    }

  protected:
    enum class FATType { FAT12, FAT16, FAT32 };
    FATType fat_type_;
    uint16_t bytes_per_sector_;
    uint8_t sectors_per_cluster_;
    uint32_t reserved_sector_count_;
    uint32_t fat_sector_count_;
    uint32_t root_dir_sector_count_;
    uint32_t data_sector_count_;
    uint32_t sector_count_per_fat_;
    uint32_t count_of_clusters_;
    uint8_t number_of_fats_;

  public:
    template <StringConvertible T>
    FATManager(T &&file_path) : file_path_(std::forward<T>(file_path)) {
        auto diskimg = file_path.c_str();
        // open the disk image as read-write
        int fd = open(diskimg, O_RDWR);
        if (fd < 0) {
            perror("open");
            exit(1);
        }
        off_t size = lseek(fd, 0, SEEK_END);
        if (size == -1) {
            perror("lseek");
            exit(1);
        }
        this->image_size_ = size;

        // mmap in READ-WRITE mode
        // image_ = static_cast<uint8_t *>(
        //     mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0));
        image_ =
            static_cast<uint8_t *>(mmap(NULL, size, O_RDWR, MAP_SHARED, fd, 0));
        if (image_ == (void *)-1) {
            perror("mmap");
            exit(1);
        }
        close(fd);

        auto hdr = reinterpret_cast<const struct BPB *>(image_);
        InitBPB(*hdr);
    }

    ~FATManager() {
        if (image_ != nullptr) {
            munmap((void *)image_, image_size_);
        }
    }

    void Ls();

    void Ck();

    void CopyFileTo(const std::string &path, const std::string &dest);

    void CopyFileFrom(const std::string &path, const std::string &dest);

    void Delete(const std::string &path);

  private:
    OptionalRef<SimpleStruct> FindFile(const std::string &path);

    std::optional<std::vector<std::reference_wrapper<SimpleStruct>>>
    FindFileWithDirs(const std::string &path);

    void DeleteSingleFile(const SimpleStruct &file);

    void DeleteSingleDir(const SimpleStruct &dir);

    void RemoveEntryInDir(const SimpleStruct &dir, const SimpleStruct &file);

    inline const std::string Info() const {
        std::string ret;
        switch (fat_type_) {
        case FATType::FAT12:
            ret += "FAT12";
            break;
        case FATType::FAT16:
            ret += "FAT16";
            break;
        case FATType::FAT32:
            ret += "FAT32";
            break;
        }
        ret += " filesystem\n";
        ret += "BytsPerSec = " + std::to_string(bytes_per_sector_) + "\n";
        ret += "SecPerClus = " + std::to_string(sectors_per_cluster_) + "\n";
        ret += "RsvdSecCnt = " + std::to_string(reserved_sector_count_) + "\n";
        ret += "FATsSecCnt = " + std::to_string(fat_sector_count_) + "\n";
        ret += "RootSecCnt = " + std::to_string(root_dir_sector_count_) + "\n";
        ret += "DataSecCnt = " + std::to_string(data_sector_count_);
        return ret;
    }

    inline const uint8_t *StartAddressOfSector(uint32_t sector_number) const {
        return image_ + (sector_number * bytes_per_sector_);
    }

    inline uint32_t MaximumValidClusterNumber() const {
        return count_of_clusters_ + 1;
    }

    void InitBPB(const BPB &bpb);

    inline uint32_t FirstSectorNumberOfDataCluster(uint32_t cluster_number) {
        ASSERT(cluster_number >= 2);
        ASSERT(cluster_number <= MaximumValidClusterNumber());

        auto first_data_sector =
            reserved_sector_count_ + (number_of_fats_ * sector_count_per_fat_);

        return (cluster_number - 2) * sectors_per_cluster_ + first_data_sector;
    }

    inline bool IsEndOfFile(uint32_t fat_entry_value) const {
        ASSERT(fat_type_ == FATType::FAT32);
        return fat_entry_value >= 0x0FFFFFF8;
    }

    inline void DecreaseFreeClusterCount(uint32_t number) {
        this->fs_info_manager_->SetFreeClusterCount(
            this->fs_info_manager_->GetFreeClusterCount() - number);
    }

    inline void IncreaseFreeClusterCount(uint32_t number) {
        this->fs_info_manager_->SetFreeClusterCount(
            this->fs_info_manager_->GetFreeClusterCount() + number);
    }

    inline std::vector<uint32_t> ClustersOfFile(const SimpleStruct &file) {
        std::vector<uint32_t> cluster_entries;
        ForEveryClusterOfFile(file, [this, &cluster_entries](uint32_t cluster) {
            cluster_entries.push_back(cluster);
        });
        return cluster_entries;
    }
};

} // namespace cs5250
